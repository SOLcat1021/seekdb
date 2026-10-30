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

#define USING_LOG_PREFIX SQL_REWRITE
#include "sql/rewrite/ob_transform_utils.h"
#include "sql/rewrite/ob_transform_simplify_orderby.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
using namespace oceanbase::sql;

/*@brief, remove_stmt_order_by removes order by from stmt that can be removed, eg:
* select * from t1 where c1 in (select c1 from t2 order by c2);
* ==>
* select * from t1 where c1 in (select c1 from t2);
*
* select distinct c1 from (select c1 from t1 order by c2);
* ==>
* select distinct c1 from (select c1 from t1);
*
* select  * from (select * from t1 order by c1) order by c2;
* ==>
* select  * from (select * from t1) order by c2;
*
* select c1 from (select c1 from t1 order by c2) group by c1;
* ==>
* select c1 from (select c1 from t1) group by c1;
*
* select c1 from t1 where c2 in (select c2 from t2 order by c1);
* ==>
* select c1 from t1 where c2 in (select c2 from t2);
*/
int ObTransformSimplifyOrderby::transform_one_stmt(common::ObIArray<ObParentDMLStmt> &parent_stmts,
                                                   ObDMLStmt *&stmt,
                                                   bool &trans_happened)
{
  int ret = OB_SUCCESS;
  bool remove_duplicates = false;
  bool subquery_happened = false;
  bool view_happened = false;
  bool set_happened = false;
  bool force_serial_set_order = false;
  trans_happened = false;
  UNUSED(parent_stmts);
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_) || OB_ISNULL(ctx_->session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ctx_->session_info_->is_serial_set_order_forced(force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_for_subquery(stmt, subquery_happened))) {
  } else if (OB_FAIL(remove_order_by_for_view_stmt(stmt, view_happened, force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_for_set_stmt(stmt, set_happened, force_serial_set_order))) {
  } else if (OB_FAIL(remove_order_by_duplicates(stmt, remove_duplicates))) {
  } else {
    trans_happened = (subquery_happened || view_happened || remove_duplicates || set_happened);
    OPT_TRACE("remove order by for subquery:", subquery_happened);
    OPT_TRACE("remove order by for view:", view_happened);
    OPT_TRACE("remove order by duplicates:", remove_duplicates);
    OPT_TRACE("remove order by for set stmt:", set_happened);
  }
  if (OB_SUCC(ret) && trans_happened) {
    if (OB_FAIL(add_transform_hint(*stmt))) {
    }
  }
  return ret;
}

// for select/update/delete/insert
// Remove order by clause from subquery
int ObTransformSimplifyOrderby::remove_order_by_for_subquery(ObDMLStmt *stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObIArray<ObQueryRefRawExpr*> &subquery_exprs = stmt->get_subquery_exprs();
    ObSelectStmt *subquery = NULL;
    bool happened = false;
    for (int64_t i = 0; OB_SUCC(ret) && i < subquery_exprs.count(); ++i) {
      ObQueryRefRawExpr *query_ref = subquery_exprs.at(i);
      if (OB_ISNULL(query_ref) || OB_ISNULL(subquery = query_ref->get_ref_stmt())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!subquery->has_limit() &&
                 !subquery->is_contains_assignment() &&
                 subquery->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(subquery, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_for_view_stmt(ObDMLStmt *stmt, bool &trans_happened, bool &force_serial_set_order, const bool count_limited_only)
{
  int ret = OB_SUCCESS;
  bool can_remove = false;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  //select stmt cannot have window function,eg:
  // SELECT last_value(c1) OVER (PARTITION BY c2) FROM (SELECT * FROM t1 ORDER BY c1, c2)s
  // ORDER BY c1, c2;
  } else if (!stmt->is_sel_del_upd()) {
    /*do nothing*/
  } else if (stmt->is_select_stmt() && static_cast<ObSelectStmt*>(stmt)->has_window_function()) {
    /*do nothing*/
  } else {
    can_remove = true;
  }
  if (OB_SUCC(ret) && can_remove) {
    ObSelectStmt *view_stmt = NULL;
    ObSelectStmt *select_stmt = NULL;
    bool happened = false;
    ObIArray<TableItem *> &table_items = stmt->get_table_items();
    for (int64_t i = 0; OB_SUCC(ret) && i < table_items.count(); ++i) {
      const TableItem * table_item = table_items.at(i);
      if (OB_ISNULL(table_item)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (!table_item->is_generated_table() &&
                 !table_item->is_lateral_table()) {
        /*do nothing*/
      } else if (OB_ISNULL(view_stmt = table_item->ref_query_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (view_stmt->is_contains_assignment() ||
                 (force_serial_set_order && view_stmt->is_set_stmt())) {
        // do nothing
      } else if (1 == table_items.count()) {
        if (stmt->is_select_stmt()) {
          select_stmt = static_cast<ObSelectStmt*>(stmt);
          // A sole COUNT(*) observes the number of rows, not which rows an
          // ordinary LIMIT/OFFSET selects. Preserve the limit but avoid sorting
          // a flat, deterministic view just to discard every selected value.
          const ObAggFunRawExpr *count_expr = 1 == select_stmt->get_aggr_item_size()
              ? select_stmt->get_aggr_item(0) : nullptr;
          const TableItem *input = 1 == view_stmt->get_table_size()
              ? view_stmt->get_table_item(0) : nullptr;
          bool count_limited_view = table_item->is_generated_table()
              && select_stmt->is_single_table_stmt() && select_stmt->is_scala_group_by()
              && !select_stmt->is_set_stmt() && !select_stmt->has_distinct()
              && !select_stmt->has_having() && !select_stmt->has_limit() && !select_stmt->has_order_by()
              && !select_stmt->has_for_update() && !select_stmt->is_contains_assignment()
              && select_stmt->get_condition_exprs().empty() && select_stmt->get_semi_infos().empty()
              && 0 == select_stmt->get_subquery_expr_size()
              && 1 == select_stmt->get_select_item_size() && nullptr != count_expr
              && select_stmt->get_select_item(0).expr_ == count_expr
              && T_FUN_COUNT == count_expr->get_expr_type() && !count_expr->is_param_distinct()
              && 0 == count_expr->get_real_param_count()
              && view_stmt->is_single_table_stmt() && nullptr != input && input->is_basic_table()
              && view_stmt->has_limit() && !view_stmt->is_fetch_with_ties()
              && nullptr == view_stmt->get_limit_percent_expr()
              && !view_stmt->has_group_by() && !view_stmt->has_distinct()
              && !view_stmt->has_having() && !view_stmt->has_window_function()
              && !view_stmt->has_for_update() && !view_stmt->has_recursive_cte()
              && 0 == view_stmt->get_subquery_expr_size();
          // For fulltext views, require one unboosted natural-language
          // membership predicate. Residual base-table filters can still need
          // lookup/scoring and must retain their original plan choices.
          if (count_limited_view && !view_stmt->get_match_exprs().empty()) {
            count_limited_view = 1 == view_stmt->get_match_exprs().count()
                && 1 == view_stmt->get_condition_exprs().count();
            if (count_limited_view) {
              const ObMatchFunRawExpr *match = view_stmt->get_match_exprs().at(0);
              count_limited_view = nullptr != match && !match->is_es_match()
                  && NATURAL_LANGUAGE_MODE == match->get_mode_flag() && match->get_columns_boosts().empty();
            }
          }
          // Do not suppress errors or effects from arbitrary discarded
          // projections/order expressions. MATCH is safe here only when the
          // same expression must already be evaluated as a WHERE predicate.
          for (int64_t j = 0; count_limited_view && j < view_stmt->get_select_item_size(); ++j) {
            const ObRawExpr *expr = view_stmt->get_select_item(j).expr_;
            count_limited_view = nullptr != expr
                && (expr->is_column_ref_expr() || expr->is_const_raw_expr());
          }
          for (int64_t j = 0; count_limited_view && j < view_stmt->get_order_item_size(); ++j) {
            const ObRawExpr *expr = view_stmt->get_order_item(j).expr_;
            bool safe = nullptr != expr && (expr->is_column_ref_expr() || expr->is_const_raw_expr());
            for (int64_t k = 0; !safe && nullptr != expr && T_FUN_MATCH_AGAINST == expr->get_expr_type()
                && k < view_stmt->get_condition_exprs().count(); ++k) {
              const ObRawExpr *condition = view_stmt->get_condition_exprs().at(k);
              safe = condition == expr || (nullptr != condition && T_OP_BOOL == condition->get_expr_type()
                  && 1 == condition->get_param_count() && condition->get_param_expr(0) == expr);
            }
            count_limited_view = safe;
          }
          if (count_limited_view && OB_FAIL(view_stmt->check_relation_exprs_deterministic(count_limited_view))) {
          } else if (((!count_limited_only && (select_stmt->has_group_by() || select_stmt->has_distinct() ||
                     select_stmt->has_order_by()) && !view_stmt->has_limit()) || count_limited_view)
                     && view_stmt->has_order_by() &&
                     OB_FAIL(do_remove_stmt_order_by(view_stmt, happened))) {
          } else {
            trans_happened |= happened;
          }
          if (OB_SUCC(ret) && count_limited_view && happened && !view_stmt->has_order_by()) {
            // COUNT(*) has no use for projected values either. Use the normal
            // view-pruning utility so parent column references stay consistent
            // and an all-pruned view receives its standard constant output.
            ObSqlBitSet<> removed_idxs;
            for (int64_t j = 0; OB_SUCC(ret) && j < view_stmt->get_select_item_size(); ++j) {
              if (nullptr == select_stmt->get_column_item_by_id(table_item->table_id_, j + OB_APP_MIN_COLUMN_ID)) {
                ret = removed_idxs.add_member(j);
              }
            }
            if (OB_SUCC(ret) && !removed_idxs.is_empty()) {
              if (OB_FAIL(ObTransformUtils::remove_select_items(
                      ctx_, table_item->table_id_, *view_stmt, *select_stmt, removed_idxs))) {
              } else if (OB_FAIL(view_stmt->formalize_stmt(ctx_->session_info_))) {
              }
            }
          }
        }
      } else if (!count_limited_only && !view_stmt->has_limit() && view_stmt->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(view_stmt, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::do_remove_stmt_order_by(ObSelectStmt *select_stmt, bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  ObSEArray<OrderItem, 4> new_order_items;
  if (OB_ISNULL(select_stmt)) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_order_item_size(); ++i) {
    const OrderItem &order_item = select_stmt->get_order_item(i);
    if (OB_ISNULL(order_item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (order_item.expr_->has_flag(CNT_SUB_QUERY)) {
      // in case the subquery returns more than one rows,
      // we should keep the subquery which may returns error
      if (OB_FAIL(new_order_items.push_back(order_item))) {
      }
    }
  }
  if (OB_SUCC(ret) && select_stmt->get_order_item_size() != new_order_items.count()) {
    trans_happened = true;
    if (OB_FAIL(select_stmt->get_order_items().assign(new_order_items))) {
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_duplicates(ObDMLStmt *stmt,
                                                           bool &trans_happened)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!stmt->is_sel_del_upd() ||
             (stmt->is_select_stmt() && (static_cast<ObSelectStmt*>(stmt)->has_rollup()))) {
    //do nothing
  } else {
    ObIArray<OrderItem> &order_items = stmt->get_order_items();
    ObIArray<ObRawExpr*> &cond_exprs = stmt->get_condition_exprs();
    if (order_items.count() > 1) {
      ObArray<OrderItem> new_order_items;
      int64_t N = order_items.count();
      int64_t M = cond_exprs.count();
      bool is_exist = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < N; ++i) {
        OrderItem &cur_item = order_items.at(i);
        is_exist = false;
        if (OB_FAIL(exist_item_by_expr(cur_item.expr_, new_order_items, is_exist))) {
        } else if (!is_exist) {
          for (int64_t j = 0; OB_SUCC(ret) && !is_exist && j < M; ++j) {
            ObRawExpr *op_expr = cond_exprs.at(j);
            if (OB_ISNULL(op_expr)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (T_OP_EQ == op_expr->get_expr_type()) {
              if (op_expr->get_param_count() != 2) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                ObRawExpr *left_param = op_expr->get_param_expr(0);
                ObRawExpr *right_param = op_expr->get_param_expr(1);
                bool is_consistent = false;
                if (OB_ISNULL(left_param) || OB_ISNULL(right_param)) {
                 ret = OB_ERR_UNEXPECTED;
                } else if (OB_FAIL(ObRelationalExprOperator::is_equal_transitive(
                                              left_param->get_result_type(),
                                              right_param->get_result_type(), is_consistent))) {
                } else if (!is_consistent) {
                  // do nothing
                } else if (right_param == cur_item.expr_) {
                  if (OB_FAIL(exist_item_by_expr(left_param, new_order_items, is_exist))) {
                  }
                } else if (left_param == cur_item.expr_) {
                  if (OB_FAIL(exist_item_by_expr(right_param, new_order_items, is_exist))) {
                  }
                }
              }
            } else {/* do nothing */}
          }
        } else {/*do nothing*/}
        if (OB_SUCC(ret) && !is_exist) {
          if (OB_FAIL(new_order_items.push_back(cur_item))) {
          }
        }
      }// for
      if (OB_SUCC(ret)) {
        if (N != new_order_items.count()) {
          trans_happened = true;
        }
        if (OB_FAIL(order_items.assign(new_order_items))) {
        }
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::exist_item_by_expr(ObRawExpr *expr, ObIArray<OrderItem> &order_items, bool &is_exist)
{
  int ret = OB_SUCCESS;
  is_exist = false;
  int64_t N = order_items.count();
  if (OB_ISNULL(expr)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < N && !is_exist; ++i) {
      OrderItem& this_item = order_items.at(i);
      if (OB_ISNULL(this_item.expr_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (this_item.expr_ == expr) {
        is_exist = true;
      } else if (this_item.expr_->same_as(*expr)) {
        is_exist = true;
      }
    }
  }
  return ret;
}

int ObTransformSimplifyOrderby::remove_order_by_for_set_stmt(ObDMLStmt *&stmt, bool &trans_happened, bool &force_serial_set_order)
{
  int ret = OB_SUCCESS;
  trans_happened = false;
  if (OB_ISNULL(stmt) || OB_ISNULL(ctx_) || OB_ISNULL(ctx_->session_info_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!(stmt->is_select_stmt() && static_cast<ObSelectStmt*>(stmt)->is_set_stmt())) {
    //not set stmt, do nothing
  } else if (force_serial_set_order && 
             ObSelectStmt::UNION == static_cast<ObSelectStmt*>(stmt)->get_set_op() &&
             !static_cast<ObSelectStmt*>(stmt)->is_set_distinct()) {
    // When force_serial_set_order is set, UNION ALL keeps order items in child statements.
  } else {
    ObIArray<ObSelectStmt*> &child_stmts = static_cast<ObSelectStmt*>(stmt)->get_set_query();
    ObSelectStmt *child_stmt = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && i < child_stmts.count(); ++i) {
      bool happened = false;
      if (OB_ISNULL(child_stmt = child_stmts.at(i))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (child_stmt->is_contains_assignment()) {
        // do nothing
      } else if (!child_stmt->has_limit() &&
                 child_stmt->has_order_by() &&
                 OB_FAIL(do_remove_stmt_order_by(child_stmt, happened))) {
      } else {
        trans_happened |= happened;
      }
    }
  }
  return ret;
}
