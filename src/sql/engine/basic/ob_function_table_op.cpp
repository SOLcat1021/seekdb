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

#include "sql/engine/basic/ob_function_table_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "common/timezone/ob_time_convert.h"
#include <cerrno>
#include <cstdlib>
#include <string>


namespace oceanbase
{
using namespace common;
namespace sql
{

OB_SERIALIZE_MEMBER((ObFunctionTableSpec, ObOpSpec), value_expr_, column_exprs_, has_correlated_expr_,
                    file_sql_kind_, file_sql_path_, file_sql_header_, file_sql_delimiter_,
                    file_sql_quote_, file_sql_null_text_, file_sql_size_, file_sql_mtime_,
                    file_sql_device_, file_sql_inode_);

int ObFunctionTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  already_calc_ = false;
  if (MY_SPEC.file_sql_kind_ >= 1 && MY_SPEC.file_sql_kind_ <= 3) {
    std::string error;
    std::string path(MY_SPEC.file_sql_path_.ptr(), MY_SPEC.file_sql_path_.length());
    file_sql::CsvOptions options;
    options.header = MY_SPEC.file_sql_header_;
    options.delimiter = MY_SPEC.file_sql_delimiter_;
    options.quote = MY_SPEC.file_sql_quote_;
    if (!MY_SPEC.file_sql_null_text_.empty()) {
      options.null_text.assign(MY_SPEC.file_sql_null_text_.ptr(), MY_SPEC.file_sql_null_text_.length());
    }
    const bool csv = MY_SPEC.file_sql_kind_ == 1;
    const bool opened = csv ? csv_reader_.open(path, options, error) :
                              json_reader_.open(path, error);
    const bool same_file = opened &&
        (csv ? csv_reader_.file_size() : json_reader_.file_size()) == MY_SPEC.file_sql_size_ &&
        (csv ? csv_reader_.file_mtime() : json_reader_.file_mtime()) == MY_SPEC.file_sql_mtime_ &&
        (csv ? csv_reader_.file_device() : json_reader_.file_device()) == MY_SPEC.file_sql_device_ &&
        (csv ? csv_reader_.file_inode() : json_reader_.file_inode()) == MY_SPEC.file_sql_inode_;
    if (opened && !same_file) error = "file changed since schema inference";
    const bool inferred = same_file &&
        (csv ? csv_reader_.infer_schema(error) : json_reader_.infer_schema(error));
    const size_t column_count = csv ? csv_reader_.columns().size() : json_reader_.columns().size();
    if (!inferred || column_count != static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
      ret = OB_ERR_PARSE_SQL;
      LOG_WARN("failed to open file SQL scan", K(ret), K(error.c_str()));
    } else {
      next_row_func_ = &ObFunctionTableOp::inner_get_next_row_file_sql;
    }
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType == MY_SPEC.value_expr_->datum_meta_.type_) {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_udf;
  } else {
    next_row_func_ = &ObFunctionTableOp::inner_get_next_row_sys_func;
  }
  return ret;
}

int ObFunctionTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    node_idx_ = 0;
    if (MY_SPEC.file_sql_kind_ >= 1 && MY_SPEC.file_sql_kind_ <= 3) {
      std::string error;
      if (!(MY_SPEC.file_sql_kind_ == 1 ? csv_reader_.rewind(error) : json_reader_.rewind(error))) {
        ret = OB_ERR_PARSE_SQL;
        LOG_WARN("failed to rewind file SQL scan", K(ret), K(error.c_str()));
      }
    }
    if (MY_SPEC.has_correlated_expr_) {
      row_count_ = 0;
      col_count_ = 0;
      value_table_ = NULL;
      already_calc_ = false;
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  node_idx_ = 0;
  row_count_ = 0;
  col_count_ = 0;
  value_table_ = NULL;
  csv_reader_.close();
  json_reader_.close();
  return ret;
}

void ObFunctionTableOp::destroy()
{
  ObOperator::destroy();
}


int ObFunctionTableOp::get_current_result(ObObj &result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;
  CK (already_calc_);
  if (node_idx_ < 0 || node_idx_ >= row_count_ * col_count_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to get current result in table function",
              K(node_idx_), K(row_count_), K(col_count_));
  }
  do {
    CK (node_idx_ >= 0);
    if (OB_SUCC(ret) && node_idx_ >= row_count_) {
      ret = OB_ITER_END;
    }
    CK (OB_NOT_NULL(value_table_));
    OX (data = value_table_->get_data());
    CK (OB_NOT_NULL(data));
    OX (result = (static_cast<ObObj*>(data))[node_idx_++]);
  } while (OB_SUCC(ret) && result.get_meta().get_type() == ObMaxType);
  return ret;
}

int ObFunctionTableOp::inner_get_next_row()
{
  return (this->*next_row_func_)();
}

int ObFunctionTableOp::inner_get_next_row_file_sql()
{
  int ret = OB_SUCCESS;
  bool end = false;
  std::string error;
  clear_evaluated_flag();
  if (OB_FAIL(ctx_.check_status())) {
  } else if (!(MY_SPEC.file_sql_kind_ == 1 ?
               csv_reader_.next(csv_values_, csv_nulls_, end, error) :
               json_reader_.next(csv_values_, csv_nulls_, end, error))) {
    ret = OB_ERR_PARSE_SQL;
    LOG_WARN("failed to read file SQL row", K(ret), K(error.c_str()));
  } else if (end) {
    ret = OB_ITER_END;
  } else if (csv_values_.size() != static_cast<size_t>(MY_SPEC.column_exprs_.count())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < MY_SPEC.column_exprs_.count(); ++i) {
      ObExpr *expr = MY_SPEC.column_exprs_.at(i);
      if (OB_ISNULL(expr)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
        ObObj obj;
        const std::string &value = csv_values_[i];
        if (csv_nulls_[i]) {
          datum.set_null();
        } else {
          switch (expr->obj_meta_.get_type()) {
            case ObTinyIntType:
              obj.set_tinyint(value.size() == 4 &&
                              (value[0] == 't' || value[0] == 'T') ? 1 : 0);
              break;
            case ObIntType: {
              char *end_ptr = nullptr;
              errno = 0;
              const int64_t parsed = std::strtoll(value.c_str(), &end_ptr, 10);
              if (errno == ERANGE || end_ptr != value.c_str() + value.size()) {
                ret = OB_ERR_PARSE_SQL;
              } else {
                obj.set_int(parsed);
              }
              break;
            }
            case ObDoubleType: {
              char *end_ptr = nullptr;
              errno = 0;
              const double parsed = std::strtod(value.c_str(), &end_ptr);
              if (errno == ERANGE || end_ptr != value.c_str() + value.size()) {
                ret = OB_ERR_PARSE_SQL;
              } else {
                obj.set_double(parsed);
              }
              break;
            }
            case ObDateType: {
              int32_t parsed = 0;
              ObDateSqlMode mode;
              mode.init(0);
              if (OB_FAIL(ObTimeConverter::str_to_date(
                      ObString(static_cast<int32_t>(value.size()), value.data()), parsed, mode))) {
              } else {
                obj.set_date(parsed);
              }
              break;
            }
            case ObDateTimeType: {
              int64_t parsed = 0;
              int16_t scale = 0;
              ObDateSqlMode mode;
              mode.init(0);
              ObTimeConvertCtx convert_ctx(nullptr, false);
              if (OB_FAIL(ObTimeConverter::str_to_datetime(
                      ObString(static_cast<int32_t>(value.size()), value.data()), convert_ctx,
                      parsed, &scale, mode))) {
              } else {
                obj.set_datetime(parsed);
              }
              break;
            }
            default:
              obj.set_varchar(value.data(), static_cast<ObString::obstr_size_t>(value.size()));
              obj.set_collation_type(expr->obj_meta_.get_collation_type());
              break;
          }
          if (OB_SUCC(ret) && OB_FAIL(datum.from_obj(obj, expr->obj_datum_map_))) {
          }
        }
        if (OB_SUCC(ret)) expr->set_evaluated_projected(eval_ctx_);
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_udf()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(MY_SPEC.value_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObExtendType != MY_SPEC.value_expr_->datum_meta_.type_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (FALSE_IT(plan_ctx->set_autoinc_id_tmp(0))) {
  } else if (OB_FAIL(ctx_.check_status())) {
  } else {
    ObDatum *value = nullptr;
    if (!already_calc_) {
      if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
      } else if (value->is_null()) {
        //do nothing
      } else if (OB_ISNULL(value_table_ 
                 = reinterpret_cast<pl::ObPLCollection*>(value->get_ext()))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        row_count_ = value_table_->is_inited() ? value_table_->get_count() : 0;
        col_count_ = value_table_->get_column_count();
      }
      already_calc_ = true;
    }
    if (OB_SUCC(ret) && OB_UNLIKELY(node_idx_ >= row_count_)) {
      ret = OB_ITER_END;
    }
    CK (MY_SPEC.column_exprs_.count() >= col_count_);
    ObObj obj_stack[col_count_];
    if (OB_SUCC(ret)) {
      if (nullptr != value_table_ 
          && ObExtendType == value_table_->get_element_type().get_obj_type()) {
        pl::ObPLComposite *composite = NULL;
        pl::ObPLRecord *record = NULL;
        ObObj record_obj;
        OZ (get_current_result(record_obj));
        if (OB_FAIL(ret)) {
        } else if (ObUserDefinedSQLType == record_obj.get_type()) {
          obj_stack[0] = record_obj;   
        } else if (record_obj.is_pl_extend()) {
          CK (OB_NOT_NULL(composite = reinterpret_cast<pl::ObPLComposite*>(record_obj.get_ext())));
          if (OB_SUCC(ret)) {
            if (composite->is_record()) {
              OX (record = static_cast<pl::ObPLRecord*>(composite));
              CK (record->get_count() == col_count_);
              for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
                OZ (record->get_element(i, obj_stack[i]));
              }
            } else if (composite->is_collection()) {
              OX (obj_stack[0] = record_obj);
            } else {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_ERROR("unexpected here", K(ret), K(record_obj), K(record_obj.meta_));
        }
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
          if (OB_FAIL(get_current_result(obj_stack[i]))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; OB_SUCC(ret) && i < col_count_; ++i) {
        if (obj_stack[i].is_null()) {
          MY_SPEC.column_exprs_.at(i)->locate_datum_for_write(eval_ctx_).set_null();
        } else {
          const ObObjDatumMapType &datum_map = MY_SPEC.column_exprs_.at(i)->obj_datum_map_;
          ObExpr * const &expr = MY_SPEC.column_exprs_.at(i);
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          if (OB_FAIL(datum.from_obj(obj_stack[i], datum_map))) {
          } else if (is_lob_storage(obj_stack[i].get_type()) &&
                     OB_FAIL(ob_adjust_lob_datum(get_exec_ctx(), obj_stack[i],
                                                 expr->obj_meta_, datum_map,
                                                 get_exec_ctx().get_allocator(), datum))) {
          }
        }
        if (OB_SUCC(ret)) {
          MY_SPEC.column_exprs_.at(i)->set_evaluated_projected(eval_ctx_);
        }
      }
    }
  }
  return ret;
}

int ObFunctionTableOp::inner_get_next_row_sys_func()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = nullptr;
  ObDatum *value = nullptr;
  clear_evaluated_flag();
  if (OB_ISNULL(plan_ctx = ctx_.get_physical_plan_ctx())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_.check_status())) {
  } else if (OB_FAIL(MY_SPEC.value_expr_->eval(eval_ctx_, value))) {
    if (OB_ITER_END != ret) {
    }
  } else {
    MY_SPEC.column_exprs_.at(0)->locate_datum_for_write(eval_ctx_).set_datum(*value);
    MY_SPEC.column_exprs_.at(0)->set_evaluated_projected(eval_ctx_);
  }
  return ret;
}


} // end namespace sql
} // end namespace oceanbase
