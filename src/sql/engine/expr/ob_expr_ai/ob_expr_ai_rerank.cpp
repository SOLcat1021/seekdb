/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include <algorithm>
#include <cmath>
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res)
{
  int ret = OB_SUCCESS;
  ObDatum *model_arg = nullptr;
  ObDatum *query_arg = nullptr;
  ObDatum *documents_arg = nullptr;
  ObDatum *key_arg = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, model_arg, query_arg, documents_arg, key_arg))) {
  } else if (model_arg->is_null() || query_arg->is_null() || documents_arg->is_null()) {
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard guard(ctx);
    MultimodeAlloctor allocator(guard.get_allocator());
    ObString model_name = model_arg->get_string();
    ObString query = query_arg->get_string();
    const bool objects = OB_NOT_NULL(key_arg) && !key_arg->is_null();
    ObString key = objects ? key_arg->get_string() : ObString();
    ObIJsonBase *root = nullptr;
    bool is_null = false;
    ObJsonArray *documents = nullptr;
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo endpoint;
    auto *resolver = share::server_service<query::ObIAiEndpointResolver>();
    if (model_name.empty() || query.empty() || (objects && key.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, allocator,
                       DOCUMENTS_IDX, root, is_null))) {
    } else if (OB_ISNULL(root) || root->json_type() != ObJsonNodeType::J_ARRAY
               || root->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_ISNULL(resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
    } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint))) {
    } else {
      documents = static_cast<ObJsonArray *>(root);
      struct Ranked { int64_t index; double score; };
      ObArray<Ranked> ranked;
      ObAIFuncModel model(allocator, *info, endpoint);
      for (int64_t first = 0; OB_SUCC(ret) && first < documents->element_count(); first += 20) {
        ObJsonArray *batch = nullptr;
        ObJsonArray *response = nullptr;
        const int64_t count = std::min<int64_t>(20, documents->element_count() - first);
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, batch))) {
        }
        for (int64_t j = 0; OB_SUCC(ret) && j < count; ++j) {
          ObJsonNode *input = documents->get_value(first + j);
          if (objects) {
            if (OB_ISNULL(input) || input->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              input = static_cast<ObJsonObject *>(input)->get_value(key);
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(input) || input->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(batch->append(input))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_rerank(query, batch, response))) {
        } else if (OB_ISNULL(response) || response->element_count() != count) {
          ret = OB_INVALID_DATA;
        } else {
          bool seen[20] = {};
          for (int64_t j = 0; OB_SUCC(ret) && j < count; ++j) {
            ObJsonNode *entry = response->get_value(j);
            if (OB_ISNULL(entry) || entry->json_type() != ObJsonNodeType::J_OBJECT) {
              ret = OB_INVALID_DATA;
            } else {
              auto *obj = static_cast<ObJsonObject *>(entry);
              auto *index = obj->get_value("index");
              auto *score = obj->get_value("relevance_score");
              double numeric = 0;
              if (OB_ISNULL(index) || (index->json_type() != ObJsonNodeType::J_INT
                  && index->json_type() != ObJsonNodeType::J_UINT) || OB_ISNULL(score)
                  || (score->json_type() != ObJsonNodeType::J_INT
                  && score->json_type() != ObJsonNodeType::J_UINT
                  && score->json_type() != ObJsonNodeType::J_DOUBLE
                  && score->json_type() != ObJsonNodeType::J_DECIMAL)) {
                ret = OB_INVALID_DATA;
              } else {
                uint64_t idx = index->json_type() == ObJsonNodeType::J_UINT
                    ? index->get_uint() : index->get_int();
                if (idx >= static_cast<uint64_t>(count) || seen[idx]) {
                  ret = OB_INVALID_DATA;
                } else if (OB_FAIL(score->to_double(numeric))) {
                } else if (!std::isfinite(numeric)) {
                  ret = OB_INVALID_DATA;
                } else {
                  seen[idx] = true;
                  if (OB_FAIL(ranked.push_back({first + static_cast<int64_t>(idx), numeric}))) {
                  }
                }
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        std::sort(&ranked.at(0), &ranked.at(0) + ranked.count(), [](const Ranked &a, const Ranked &b) {
          return a.score > b.score || (a.score == b.score && a.index < b.index);
        });
        ObJsonArray *output = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, output))) {
        }
        for (const auto &item : ranked) {
          if (OB_FAIL(ret)) { break; }
          ObJsonObject *obj = nullptr;
          ObJsonDouble *score = nullptr;
          if (objects) {
            obj = static_cast<ObJsonObject *>(documents->get_value(item.index)->clone(&allocator, true));
            if (OB_ISNULL(obj)) { ret = OB_ALLOCATE_MEMORY_FAILED; }
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, obj))) {
          } else {
            ObJsonInt *index = nullptr;
            if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, item.index, index))) {
            } else if (OB_FAIL(obj->add("index", index))) {
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_ISNULL(score = OB_NEWx(ObJsonDouble, &allocator, item.score))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(obj->add(objects ? "_model_score" : "relevance_score", score))) {
          } else if (OB_FAIL(output->append(obj))) {
          }
        }
        ObString binary;
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(output->get_raw_binary(binary, &allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, binary))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
