/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Functions for operators in Cypher expressions.
 */

#include "postgres.h"
#include "varatt.h"
#include <math.h>
#include <limits.h>

#include "utils/agtype.h"
#include "utils/datum.h"
#include "utils/builtins.h"

static agtype *agtype_concat_impl(agtype *agt1, agtype *agt2);
static agtype_value *iterator_concat(agtype_iterator **it1,
                                     agtype_iterator **it2,
                                     agtype_parse_state **state);
static void concat_to_agtype_string(agtype_value *result, char *lhs, int llen,
                                    char *rhs, int rlen);
static char *get_string_from_agtype_value(agtype_value *agtv, int *length);
static Datum get_agtype_path_all(FunctionCallInfo fcinfo, bool as_text);
static agtype *delete_from_object(agtype *agt, char *keyptr, int keylen);
static agtype *delete_from_array(agtype *agt, agtype* indexes);

static void concat_to_agtype_string(agtype_value *result, char *lhs, int llen,
                                    char *rhs, int rlen)
{
    int length = llen + rlen;
    char *buffer = result->val.string.val;

    Assert(llen >= 0 && rlen >= 0);
    check_string_length(length);
    buffer = palloc(length);

    strncpy(buffer, lhs, llen);
    strncpy(buffer + llen, rhs, rlen);

    result->type = AGTV_STRING;
    result->val.string.len = length;
    result->val.string.val = buffer;
}

static char *get_string_from_agtype_value(agtype_value *agtv, int *length)
{
    Datum number;
    char *string;

    switch (agtv->type)
    {
    case AGTV_INTEGER:
        number = DirectFunctionCall1(int8out,
                                     Int8GetDatum(agtv->val.int_value));
        string = DatumGetCString(number);
        *length = strlen(string);
        return string;
    case AGTV_FLOAT:
        number = DirectFunctionCall1(float8out,
                                     Float8GetDatum(agtv->val.float_value));
        string = DatumGetCString(number);
        *length = strlen(string);

        if (is_decimal_needed(string))
        {
            char *str = palloc(*length + 2);
            strncpy(str, string, *length);
            strncpy(str + *length, ".0", 2);
            *length += 2;
            string = str;
        }
        return string;
    case AGTV_STRING:
        *length = agtv->val.string.len;
        return agtv->val.string.val;

    case AGTV_NUMERIC:
        string = DatumGetCString(DirectFunctionCall1(numeric_out,
                     PointerGetDatum(agtv->val.numeric)));
        *length = strlen(string);
        return string;

    case AGTV_NULL:
    case AGTV_BOOL:
    case AGTV_ARRAY:
    case AGTV_OBJECT:
    case AGTV_BINARY:
    default:
        *length = 0;
        return NULL;
    }
    return NULL;
}

Datum get_numeric_datum_from_agtype_value(agtype_value *agtv)
{
    switch (agtv->type)
    {
    case AGTV_INTEGER:
        return DirectFunctionCall1(int8_numeric,
                                   Int8GetDatum(agtv->val.int_value));
    case AGTV_FLOAT:
        return DirectFunctionCall1(float8_numeric,
                                   Float8GetDatum(agtv->val.float_value));
    case AGTV_NUMERIC:
        return NumericGetDatum(agtv->val.numeric);

    default:
        break;
    }

    return 0;
}

bool is_numeric_result(agtype_value *lhs, agtype_value *rhs)
{
    if (((lhs->type == AGTV_NUMERIC || rhs->type == AGTV_NUMERIC) &&
         (lhs->type == AGTV_INTEGER || lhs->type == AGTV_FLOAT ||
          rhs->type == AGTV_INTEGER || rhs->type == AGTV_FLOAT )) ||
        (lhs->type == AGTV_NUMERIC && rhs->type == AGTV_NUMERIC))
    {
        return true;
    }

    return false;
}

PG_FUNCTION_INFO_V1(agtype_add);

/* agtype addition and concat function for + operator */
Datum agtype_add(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    /* If both are not scalars */
    if (!(AGT_ROOT_IS_SCALAR(lhs) && AGT_ROOT_IS_SCALAR(rhs)))
    {
        Datum agt = AGTYPE_P_GET_DATUM(agtype_concat_impl(lhs, rhs));

        PG_RETURN_DATUM(agt);
    }

    /* Both are scalar */
    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    /*
     * One or both values is a string OR one is a string and the other is
     * either an integer, float, or numeric. If so, concatenate them.
     */
    if ((agtv_lhs->type == AGTV_STRING || agtv_rhs->type == AGTV_STRING) &&
        (agtv_lhs->type == AGTV_INTEGER || agtv_lhs->type == AGTV_FLOAT ||
         agtv_lhs->type == AGTV_NUMERIC || agtv_lhs->type == AGTV_STRING ||
         agtv_rhs->type == AGTV_INTEGER || agtv_rhs->type == AGTV_FLOAT ||
         agtv_rhs->type == AGTV_NUMERIC || agtv_rhs->type == AGTV_STRING))
    {
        int llen = 0;
        char *lhs = get_string_from_agtype_value(agtv_lhs, &llen);
        int rlen = 0;
        char *rhs = get_string_from_agtype_value(agtv_rhs, &rlen);

        concat_to_agtype_string(&agtv_result, lhs, llen, rhs, rlen);
    }
    /* Both are integers - regular addition */
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = agtv_lhs->val.int_value +
                                    agtv_rhs->val.int_value;
    }
    /* Both are floats - regular addition */
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value +
                                      agtv_rhs->val.float_value;
    }
    /* The left is a float, the right is an integer - regular addition */
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value +
                                      agtv_rhs->val.int_value;
    }
    /* The right is a float, the left is an integer - regular addition */
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.int_value +
                                      agtv_rhs->val.float_value;
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_add, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    /* if both operands are scalar(vertex/edge/path), concat the two */
    else if (AGT_ROOT_IS_SCALAR(lhs) && AGT_ROOT_IS_SCALAR(rhs))
    {
        Datum agt = AGTYPE_P_GET_DATUM(agtype_concat_impl(lhs, rhs));

        PG_RETURN_DATUM(agt);
    }
    else
    {
        /* Not a covered case, error out */
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("Invalid input parameter types for agtype_add")));
    }

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_any_add);

/* agtype addition between bigint and agtype */
Datum agtype_any_add(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_add, AGTYPE_P_GET_DATUM(lhs),
                                             AGTYPE_P_GET_DATUM(rhs));

    AG_RETURN_AGTYPE_P(DATUM_GET_AGTYPE_P(result));
}

/*
 * For the given indexes array, delete elements at those indexes
 * from the passed in agtype array.
 */
static agtype *delete_from_array(agtype *agt, agtype *indexes)
{
    agtype_parse_state *state = NULL;
    agtype_iterator *it, *it_indexes = NULL;
    uint32 i = 0, n;
    agtype_value v, *res = NULL;
    agtype_iterator_token r;

    if (!AGT_ROOT_IS_ARRAY(agt) || AGT_ROOT_IS_SCALAR(agt))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot delete from scalar or object"
                        "using integer index")));
    }

    /* array is empty, pass the original array */
    if (AGT_ROOT_COUNT(agt) == 0)
    {
        return agt;
    }

    /* start buidiling the result agtype array */
    it = agtype_iterator_init(&agt->root);

    r = agtype_iterator_next(&it, &v, false);
    Assert(r == WAGT_BEGIN_ARRAY);

    n = v.val.array.num_elems;

    push_agtype_value(&state, r, NULL);

    while ((r = agtype_iterator_next(&it, &v, true)) != WAGT_DONE)
    {
        if (r == WAGT_ELEM)
        {
            /*
             * use logic similar to agtype_contains to check
             * if the current index (itself or in inverted form)
             * is contained in the indexes array,
             * if yes, skip the element at that index in agt array
             * else add the element in result agtype array
             */
            agtype_value cur_idx, neg_idx;
            agtype *cur_idx_agt, *neg_idx_agt;
            agtype_iterator *it_cur_idx, *it_neg_idx;
            bool contains_idx, contains_neg_idx;

            cur_idx.type = AGTV_INTEGER;
            cur_idx.val.int_value = i++;
            cur_idx_agt = agtype_value_to_agtype(&cur_idx);

            neg_idx.type = AGTV_INTEGER;
            neg_idx.val.int_value = cur_idx.val.int_value - n;
            neg_idx_agt = agtype_value_to_agtype(&neg_idx);

            it_cur_idx = agtype_iterator_init(&cur_idx_agt->root);
            it_neg_idx = agtype_iterator_init(&neg_idx_agt->root);

            it_indexes = agtype_iterator_init(&indexes->root);
            contains_idx = agtype_deep_contains(&it_indexes, &it_cur_idx, false);

            /* re-initialize indexes array iterator */
            it_indexes = agtype_iterator_init(&indexes->root);
            contains_neg_idx = agtype_deep_contains(&it_indexes, &it_neg_idx, false);

            if (contains_idx || contains_neg_idx)
            {
                continue;
            }
        }

        res = push_agtype_value(&state, r, r < WAGT_BEGIN_ARRAY ? &v : NULL);
    }

    Assert(res != NULL);

    return agtype_value_to_agtype(res);
}

/*
 * For the given key delete that property from the passed in agtype
 * object.
 */
static agtype *delete_from_object(agtype *agt, char *keyptr, int keylen)
{
    agtype_parse_state *state = NULL;
    agtype_iterator *it;
    agtype_value v, *res = NULL;
    bool skipNested = false;
    agtype_iterator_token r;

    if (!AGT_ROOT_IS_OBJECT(agt))
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("cannot delete from scalar or array"
                        "using string key")));
    }

    if (AGT_ROOT_COUNT(agt) == 0)
    {
        return agt;
    }

    it = agtype_iterator_init(&agt->root);

    while ((r = agtype_iterator_next(&it, &v, skipNested)) != WAGT_DONE)
    {
        skipNested = true;

        /*
         * Checks the key to compare against the passed in key to be
         * deleted. do not add the key and value to the new agtype being
         * constructed.
         */
        if ((r == WAGT_ELEM || r == WAGT_KEY) &&
            (v.type == AGTV_STRING && keylen == v.val.string.len &&
             memcmp(keyptr, v.val.string.val, keylen) == 0))
        {
            /* skip corresponding value as well */
            if (r == WAGT_KEY)
            {
                (void) agtype_iterator_next(&it, &v, true);
            }

            continue;
        }

        res = push_agtype_value(&state, r, r < WAGT_BEGIN_ARRAY ? &v : NULL);
    }

    Assert(res != NULL);

    return agtype_value_to_agtype(res);
}

PG_FUNCTION_INFO_V1(agtype_sub);

/*
 * agtype subtraction function for - operator
 */
Datum agtype_sub(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    /*
     * Logic to handle when the rhs is a non scalar array. In this
     * case;
     * 1. if the lhs is an object, the values in the rhs array
     *    are string keys to be removed from the object.
     * 2. if the lhs is an array, the values in the rhs array
     *    are integer indexes at which values should be removed from array.
     * otherwise throw an error
     */
    if (AGT_ROOT_IS_ARRAY(rhs) && !AGT_ROOT_IS_SCALAR(rhs))
    {
        agtype_iterator *it = NULL;
        agtype_value elem;

        if (AGT_ROOT_IS_OBJECT(lhs))
        {
            /*
             * if rhs array contains any non-string element, error out
             * else delete the given keys in the rhs array from lhs object
             */
            while ((it = get_next_list_element(it, &rhs->root, &elem)))
            {
                if (elem.type == AGTV_STRING)
                {
                    lhs = delete_from_object(lhs, elem.val.string.val,
                                                elem.val.string.len);
                }
                else
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("expected agtype string, not agtype %s",
                                    agtype_value_type_to_string(elem.type))));
                }
            }
        }
        else if (AGT_ROOT_IS_ARRAY(lhs) && !(AGT_ROOT_IS_SCALAR(lhs)))
        {
            /*
             * if rhs array contains any non-integer element, error out
             * else delete the values at the given indexes in rhs array
             * from the lhs array
             */
            while ((it = get_next_list_element(it, &rhs->root, &elem)))
            {
                if (elem.type != AGTV_INTEGER)
                {
                    ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("expected agtype integer, not agtype %s",
                                    agtype_value_type_to_string(elem.type))));
                }
            }

            lhs = delete_from_array(lhs, rhs);
        }
        else
        {
            ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("must be object or array, not a scalar value")));
        }

        AG_RETURN_AGTYPE_P(lhs);
    }

    /*
     * When the lhs is an object and rhs is a string, remove the key from
     * the object.
     * When the lhs is an array and the rhs is an integer then
     * remove the value at that index from the array,
     * otherwise give an error
     */
    if(!AGT_ROOT_IS_SCALAR(lhs))
    {
        agtype_value *key;
        key = get_ith_agtype_value_from_container(&rhs->root, 0);

        if (AGT_ROOT_IS_OBJECT(lhs) && key->type == AGTV_STRING)
        {
            AG_RETURN_AGTYPE_P(delete_from_object(lhs, key->val.string.val,
                                                  key->val.string.len));
        }
        else if (AGT_ROOT_IS_ARRAY(lhs) && key->type == AGTV_INTEGER)
        {
            AG_RETURN_AGTYPE_P(delete_from_array(lhs, rhs));
        }
        else
        {
            if (AGT_ROOT_IS_OBJECT(lhs))
            {
                ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("expected agtype string, not agtype %s",
                                    agtype_value_type_to_string(key->type))));
            }
            else if (AGT_ROOT_IS_ARRAY(lhs))
            {
                ereport(ERROR,
                            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                            errmsg("expected agtype integer, not agtype %s",
                                    agtype_value_type_to_string(key->type))));
            }
        }
    }

    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = agtv_lhs->val.int_value -
                                    agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value -
                                      agtv_rhs->val.float_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value -
                                      agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.int_value -
                                      agtv_rhs->val.float_value;
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_sub, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
    {
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                errmsg("Invalid input parameter types for agtype_sub")));
    }

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_any_sub);

/* agtype subtraction between bigint and agtype */
Datum agtype_any_sub(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_sub, AGTYPE_P_GET_DATUM(lhs),
                                             AGTYPE_P_GET_DATUM(rhs));

    AG_RETURN_AGTYPE_P(DATUM_GET_AGTYPE_P(result));
}

PG_FUNCTION_INFO_V1(agtype_neg);

/*
 * agtype negation function for unary - operator
 */
Datum agtype_neg(PG_FUNCTION_ARGS)
{
    agtype *v = AG_GET_ARG_AGTYPE_P(0);
    agtype_value *agtv_value;
    agtype_value agtv_result;

    if (!(AGT_ROOT_IS_SCALAR(v)))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("must be scalar value, not array or object")));

        PG_RETURN_NULL();
    }

    agtv_value = get_ith_agtype_value_from_container(&v->root, 0);

    if (agtv_value->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = -agtv_value->val.int_value;
    }
    else if (agtv_value->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = -agtv_value->val.float_value;
    }
    else if (agtv_value->type == AGTV_NUMERIC)
    {
        Datum numd, vald;

        vald = NumericGetDatum(agtv_value->val.numeric);
        numd = DirectFunctionCall1(numeric_uminus, vald);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input parameter type for agtype_neg")));

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_mul);

/*
 * agtype multiplication function for * operator
 */
Datum agtype_mul(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    if (!(AGT_ROOT_IS_SCALAR(lhs)) || !(AGT_ROOT_IS_SCALAR(rhs)))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("must be scalar value, not array or object")));

        PG_RETURN_NULL();
    }

    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = agtv_lhs->val.int_value *
                                    agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value *
                                      agtv_rhs->val.float_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value *
                                      agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.int_value *
                                      agtv_rhs->val.float_value;
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_mul, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input parameter types for agtype_mul")));

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_any_mul);

/* agtype multiplication between bigint and agtype */
Datum agtype_any_mul(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_mul, AGTYPE_P_GET_DATUM(lhs),
                                             AGTYPE_P_GET_DATUM(rhs));

    AG_RETURN_AGTYPE_P(DATUM_GET_AGTYPE_P(result));
}

PG_FUNCTION_INFO_V1(agtype_div);

/*
 * agtype division function for / operator
 */
Datum agtype_div(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    if (!(AGT_ROOT_IS_SCALAR(lhs)) || !(AGT_ROOT_IS_SCALAR(rhs)))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("must be scalar value, not array or object")));

        PG_RETURN_NULL();
    }

    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        if (agtv_rhs->val.int_value == 0)
        {
            ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO),
                            errmsg("division by zero")));
            PG_RETURN_NULL();
        }

        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = agtv_lhs->val.int_value /
                                    agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        if (agtv_rhs->val.float_value == 0)
        {
            ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO),
                            errmsg("division by zero")));
            PG_RETURN_NULL();
        }

        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value /
                                      agtv_rhs->val.float_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        if (agtv_rhs->val.int_value == 0)
        {
            ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO),
                            errmsg("division by zero")));
            PG_RETURN_NULL();
        }

        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.float_value /
                                      agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        if (agtv_rhs->val.float_value == 0)
        {
            ereport(ERROR, (errcode(ERRCODE_DIVISION_BY_ZERO),
                            errmsg("division by zero")));
            PG_RETURN_NULL();
        }

        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = agtv_lhs->val.int_value /
                                      agtv_rhs->val.float_value;
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_div, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input parameter types for agtype_div")));

     AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_any_div);

/* agtype division between bigint and agtype */
Datum agtype_any_div(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_div, AGTYPE_P_GET_DATUM(lhs),
                                             AGTYPE_P_GET_DATUM(rhs));

    AG_RETURN_AGTYPE_P(DATUM_GET_AGTYPE_P(result));
}

PG_FUNCTION_INFO_V1(agtype_mod);

/*
 * agtype modulo function for % operator
 */
Datum agtype_mod(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    if (!(AGT_ROOT_IS_SCALAR(lhs)) || !(AGT_ROOT_IS_SCALAR(rhs)))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("must be scalar value, not array or object")));

        PG_RETURN_NULL();
    }

    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_INTEGER;
        agtv_result.val.int_value = agtv_lhs->val.int_value %
                                    agtv_rhs->val.int_value;
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = fmod(agtv_lhs->val.float_value,
                                           agtv_rhs->val.float_value);
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = fmod(agtv_lhs->val.float_value,
                                           agtv_rhs->val.int_value);
    }
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = fmod(agtv_lhs->val.int_value,
                                           agtv_rhs->val.float_value);
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_mod, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input parameter types for agtype_mod")));

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_any_mod);

/* agtype modulo between bigint and agtype */
Datum agtype_any_mod(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_mod, AGTYPE_P_GET_DATUM(lhs),
                                             AGTYPE_P_GET_DATUM(rhs));

    AG_RETURN_AGTYPE_P(DATUM_GET_AGTYPE_P(result));
}

PG_FUNCTION_INFO_V1(agtype_pow);

/*
 * agtype power function for ^ operator
 */
Datum agtype_pow(PG_FUNCTION_ARGS)
{
    agtype *lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *rhs = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *agtv_lhs;
    agtype_value *agtv_rhs;
    agtype_value agtv_result;

    if (!(AGT_ROOT_IS_SCALAR(lhs)) || !(AGT_ROOT_IS_SCALAR(rhs)))
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("must be scalar value, not array or object")));

        PG_RETURN_NULL();
    }

    agtv_lhs = get_ith_agtype_value_from_container(&lhs->root, 0);
    agtv_rhs = get_ith_agtype_value_from_container(&rhs->root, 0);

    if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = pow(agtv_lhs->val.int_value,
                                          agtv_rhs->val.int_value);
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = pow(agtv_lhs->val.float_value,
                                          agtv_rhs->val.float_value);
    }
    else if (agtv_lhs->type == AGTV_FLOAT && agtv_rhs->type == AGTV_INTEGER)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = pow(agtv_lhs->val.float_value,
                                          agtv_rhs->val.int_value);
    }
    else if (agtv_lhs->type == AGTV_INTEGER && agtv_rhs->type == AGTV_FLOAT)
    {
        agtv_result.type = AGTV_FLOAT;
        agtv_result.val.float_value = pow(agtv_lhs->val.int_value,
                                          agtv_rhs->val.float_value);
    }
    /* Is this a numeric result */
    else if (is_numeric_result(agtv_lhs, agtv_rhs))
    {
        Datum numd, lhsd, rhsd;

        lhsd = get_numeric_datum_from_agtype_value(agtv_lhs);
        rhsd = get_numeric_datum_from_agtype_value(agtv_rhs);
        numd = DirectFunctionCall2(numeric_power, lhsd, rhsd);

        agtv_result.type = AGTV_NUMERIC;
        agtv_result.val.numeric = DatumGetNumeric(numd);
    }
    else
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("Invalid input parameter types for agtype_pow")));

    AG_RETURN_AGTYPE_P(agtype_value_to_agtype(&agtv_result));
}

PG_FUNCTION_INFO_V1(agtype_eq);

Datum agtype_eq(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);
    agtype *agtype_lhs = NULL;
    agtype *agtype_rhs = NULL;
    uint32 hash_lhs = datum_image_hash(lhs, false, -1);
    uint32 hash_rhs = datum_image_hash(rhs, false, -1);
    bool result = false;

    if (hash_lhs == hash_rhs &&
        datum_image_eq(lhs, rhs, false, -1))
    {
        PG_RETURN_BOOL(true);
    }

    agtype_lhs = DATUM_GET_AGTYPE_P(lhs);
    agtype_rhs = DATUM_GET_AGTYPE_P(rhs);

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) == 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_eq);

Datum agtype_any_eq(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_eq, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_ne);

Datum agtype_ne(PG_FUNCTION_ARGS)
{
    Datum lhs = PG_GETARG_DATUM(0);
    Datum rhs = PG_GETARG_DATUM(1);
    uint32 hash_lhs = datum_image_hash(lhs, false, -1);
    uint32 hash_rhs = datum_image_hash(rhs, false, -1);
    agtype *agtype_lhs = NULL;
    agtype *agtype_rhs = NULL;
    bool result = false;

    if (hash_lhs == hash_rhs &&
        datum_image_eq(lhs, rhs, false, -1))
    {
        PG_RETURN_BOOL(false);
    }

    agtype_lhs = DATUM_GET_AGTYPE_P(lhs);
    agtype_rhs = DATUM_GET_AGTYPE_P(rhs);

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) != 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_ne);

Datum agtype_any_ne(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_ne, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_lt);

Datum agtype_lt(PG_FUNCTION_ARGS)
{
    agtype *agtype_lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *agtype_rhs = AG_GET_ARG_AGTYPE_P(1);
    bool result;

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) < 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_lt);

Datum agtype_any_lt(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_lt, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_gt);

Datum agtype_gt(PG_FUNCTION_ARGS)
{
    agtype *agtype_lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *agtype_rhs = AG_GET_ARG_AGTYPE_P(1);
    bool result;

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) > 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_gt);

Datum agtype_any_gt(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_gt, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_le);

Datum agtype_le(PG_FUNCTION_ARGS)
{
    agtype *agtype_lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *agtype_rhs = AG_GET_ARG_AGTYPE_P(1);
    bool result;

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) <= 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_le);

Datum agtype_any_le(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_le, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_ge);

Datum agtype_ge(PG_FUNCTION_ARGS)
{
    agtype *agtype_lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *agtype_rhs = AG_GET_ARG_AGTYPE_P(1);
    bool result;

    result = (compare_agtype_containers_orderability(&agtype_lhs->root,
                                                     &agtype_rhs->root) >= 0);

    PG_FREE_IF_COPY(agtype_lhs, 0);
    PG_FREE_IF_COPY(agtype_rhs, 1);

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_any_ge);

Datum agtype_any_ge(PG_FUNCTION_ARGS)
{
    agtype *lhs;
    agtype *rhs;
    Datum result;

    lhs = get_one_agtype_from_variadic_args(fcinfo, 0, 2);
    rhs = get_one_agtype_from_variadic_args(fcinfo, 1, 1);

    if (lhs == NULL || rhs == NULL)
    {
        PG_RETURN_NULL();
    }

    result = DirectFunctionCall2(agtype_ge, AGTYPE_P_GET_DATUM(lhs),
                                            AGTYPE_P_GET_DATUM(rhs));

    PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(agtype_exists_agtype);
/*
 * ? operator for agtype. Returns true if the string exists as top-level keys
 */
Datum agtype_exists_agtype(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    agtype *key = AG_GET_ARG_AGTYPE_P(1);
    agtype_value *aval;
    agtype_value *v = NULL;

    if (AGT_ROOT_IS_SCALAR(agt))
    {
        agt = agtype_value_to_agtype(extract_entity_properties(agt, false));
    }

    if (AGT_ROOT_IS_SCALAR(key))
    {
        aval = get_ith_agtype_value_from_container(&key->root, 0);
    }
    else
    {
        PG_RETURN_BOOL(false);
    }

    if (AGT_ROOT_IS_OBJECT(agt) &&
        aval->type == AGTV_STRING)
    {
        v = find_agtype_value_from_container(&agt->root,
                                             AGT_FOBJECT,
                                             aval);
    }
    else if (AGT_ROOT_IS_ARRAY(agt) &&
             aval->type != AGTV_NULL)
    {
        v = find_agtype_value_from_container(&agt->root,
                                             AGT_FARRAY,
                                             aval);
    }

    PG_RETURN_BOOL(v != NULL);
}

PG_FUNCTION_INFO_V1(agtype_exists_any_agtype);
/*
 * ?| operator for agtype. Returns true if any of the array strings exist as
 * top-level keys
 */
Datum agtype_exists_any_agtype(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    agtype *keys = AG_GET_ARG_AGTYPE_P(1);
    agtype_value elem;
    agtype_iterator *it = NULL;

    if (AGT_ROOT_IS_SCALAR(agt))
    {
        agt = agtype_value_to_agtype(extract_entity_properties(agt, true));
    }

    if (!AGT_ROOT_IS_SCALAR(keys) && !AGT_ROOT_IS_OBJECT(keys))
    {
        while ((it = get_next_list_element(it, &keys->root, &elem)))
        {
            if (IS_A_AGTYPE_SCALAR(&elem))
            {
                if (AGT_ROOT_IS_OBJECT(agt) &&
                    (&elem)->type == AGTV_STRING &&
                    find_agtype_value_from_container(&agt->root,
                                                         AGT_FOBJECT,
                                                         &elem))
                {
                    PG_RETURN_BOOL(true);
                }
                else if (AGT_ROOT_IS_ARRAY(agt) &&
                         (&elem)->type != AGTV_NULL &&
                         find_agtype_value_from_container(&agt->root,
                                                           AGT_FARRAY,
                                                           &elem))
                {
                    PG_RETURN_BOOL(true);
                }
            }
            else
            {
                PG_RETURN_BOOL(false);
            }
        }
    }
    else
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid agtype value for right operand")));
    }

    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(agtype_exists_all_agtype);
/*
 * ?& operator for agtype. Returns true if all of the array strings exist as
 * top-level keys
 */
Datum agtype_exists_all_agtype(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    agtype *keys = AG_GET_ARG_AGTYPE_P(1);
    agtype_value elem;
    agtype_iterator *it = NULL;

    if (AGT_ROOT_IS_SCALAR(agt))
    {
        agt = agtype_value_to_agtype(extract_entity_properties(agt, true));
    }

    if (!AGT_ROOT_IS_SCALAR(keys) && !AGT_ROOT_IS_OBJECT(keys))
    {
        while ((it = get_next_list_element(it, &keys->root, &elem)))
        {
            if (IS_A_AGTYPE_SCALAR(&elem))
            {
                if ((&elem)->type == AGTV_NULL)
                {
                    continue;
                }
                else if (AGT_ROOT_IS_OBJECT(agt) &&
                         (&elem)->type == AGTV_STRING &&
                         find_agtype_value_from_container(&agt->root,
                                                          AGT_FOBJECT,
                                                          &elem))
                {
                    continue;
                }
                else if (AGT_ROOT_IS_ARRAY(agt) &&
                         find_agtype_value_from_container(&agt->root,
                                                          AGT_FARRAY,
                                                          &elem))
                {
                    continue;
                }
                else
                {
                    PG_RETURN_BOOL(false);
                }
            }
            else
            {
                PG_RETURN_BOOL(false);
            }
        }
    }
    else
    {
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid agtype value for right operand")));
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(agtype_contains);
/*
 * @> operator for agtype. Returns true if the right agtype path/value entries
 * contained at the top level within the left agtype value
 */
Datum agtype_contains(PG_FUNCTION_ARGS)
{
    agtype_iterator *constraint_it = NULL;
    agtype_iterator *property_it = NULL;
    agtype *properties = NULL;
    agtype *constraints = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    properties = AG_GET_ARG_AGTYPE_P(0);
    constraints = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(properties)
            && AGTE_IS_AGTYPE(properties->root.children[0]))
    {
        properties =
            agtype_value_to_agtype(extract_entity_properties(properties,
                                                             false));
    }

    if (AGT_ROOT_IS_SCALAR(constraints)
            && AGTE_IS_AGTYPE(constraints->root.children[0]))
    {
        constraints =
            agtype_value_to_agtype(extract_entity_properties(constraints,
                                                             false));
    }

    if (AGT_ROOT_IS_OBJECT(properties) != AGT_ROOT_IS_OBJECT(constraints))
    {
        PG_RETURN_BOOL(false);
    }

    property_it = agtype_iterator_init(&properties->root);
    constraint_it = agtype_iterator_init(&constraints->root);

    PG_RETURN_BOOL(agtype_deep_contains(&property_it, &constraint_it, false));
}

PG_FUNCTION_INFO_V1(agtype_contained_by_top_level);
/*
 * Function for operator <<@
 * Works similar to <@, but unlike <@, this function does not recurse
 * into object values, instead checks if the value of top-level key in
 * right agtype is equal to the one on the left.
 */
Datum agtype_contained_by_top_level(PG_FUNCTION_ARGS)
{
    agtype_iterator *constraint_it, *property_it;
    agtype *properties, *constraints;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    properties = AG_GET_ARG_AGTYPE_P(0);
    constraints = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(properties)
            && AGTE_IS_AGTYPE(properties->root.children[0]))
    {
        properties =
            agtype_value_to_agtype(extract_entity_properties(properties,
                                                             false));
    }

    if (AGT_ROOT_IS_SCALAR(constraints)
            && AGTE_IS_AGTYPE(constraints->root.children[0]))
    {
        constraints =
            agtype_value_to_agtype(extract_entity_properties(constraints,
                                                             false));
    }

    constraint_it = agtype_iterator_init(&constraints->root);
    property_it = agtype_iterator_init(&properties->root);

    PG_RETURN_BOOL(agtype_deep_contains(&constraint_it, &property_it, true));
}


PG_FUNCTION_INFO_V1(agtype_contained_by);
/*
 * <@ operator for agtype. Returns true if the left agtype path/value entries
 * contained at the top level within the right agtype value
 */
Datum agtype_contained_by(PG_FUNCTION_ARGS)
{
    agtype_iterator *constraint_it, *property_it;
    agtype *properties, *constraints;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    properties = AG_GET_ARG_AGTYPE_P(0);
    constraints = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(properties)
            && AGTE_IS_AGTYPE(properties->root.children[0]))
    {
        properties =
            agtype_value_to_agtype(extract_entity_properties(properties,
                                                             false));
    }

    if (AGT_ROOT_IS_SCALAR(constraints)
            && AGTE_IS_AGTYPE(constraints->root.children[0]))
    {
        constraints =
            agtype_value_to_agtype(extract_entity_properties(constraints,
                                                             false));
    }

    constraint_it = agtype_iterator_init(&constraints->root);
    property_it = agtype_iterator_init(&properties->root);

    PG_RETURN_BOOL(agtype_deep_contains(&constraint_it, &property_it, false));
}

PG_FUNCTION_INFO_V1(agtype_contains_top_level);
/*
 * Function for operator @>>.
 * Works similar to @>, but unlike @>, this function does not recurse
 * into object values, instead checks if the value of top-level key in
 * left agtype is equal to the one on the right.
 */
Datum agtype_contains_top_level(PG_FUNCTION_ARGS)
{
    agtype_iterator *constraint_it = NULL;
    agtype_iterator *property_it = NULL;
    agtype *properties = NULL;
    agtype *constraints = NULL;

    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_BOOL(false);
    }

    properties = AG_GET_ARG_AGTYPE_P(0);
    constraints = AG_GET_ARG_AGTYPE_P(1);

    if (AGT_ROOT_IS_SCALAR(properties)
            && AGTE_IS_AGTYPE(properties->root.children[0]))
    {
        properties =
            agtype_value_to_agtype(extract_entity_properties(properties,
                                                             false));
    }

    if (AGT_ROOT_IS_SCALAR(constraints)
            && AGTE_IS_AGTYPE(constraints->root.children[0]))
    {
        constraints =
            agtype_value_to_agtype(extract_entity_properties(constraints,
                                                             false));
    }

    if (AGT_ROOT_IS_OBJECT(properties) != AGT_ROOT_IS_OBJECT(constraints))
    {
        PG_RETURN_BOOL(false);
    }

    property_it = agtype_iterator_init(&properties->root);
    constraint_it = agtype_iterator_init(&constraints->root);

    PG_RETURN_BOOL(agtype_deep_contains(&property_it, &constraint_it, true));
}

PG_FUNCTION_INFO_V1(agtype_exists);
/*
 * ? operator for agtype. Returns true if the string exists as top-level keys
 */
Datum agtype_exists(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    text *key = PG_GETARG_TEXT_PP(1);
    agtype_value aval;
    agtype_value *v = NULL;

    /*
     * We only match Object keys (which are naturally always Strings), or
     * string elements in arrays.  In particular, we do not match non-string
     * scalar elements.  Existence of a key/element is only considered at the
     * top level.  No recursion occurs.
     */
    aval.type = AGTV_STRING;
    aval.val.string.val = VARDATA_ANY(key);
    aval.val.string.len = VARSIZE_ANY_EXHDR(key);

    v = find_agtype_value_from_container(&agt->root,
                                         AGT_FOBJECT | AGT_FARRAY,
                                         &aval);

    PG_RETURN_BOOL(v != NULL);
}

PG_FUNCTION_INFO_V1(agtype_exists_any);
/*
 * ?| operator for agtype. Returns true if any of the array strings exist as
 * top-level keys
 */
Datum agtype_exists_any(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    ArrayType *keys = PG_GETARG_ARRAYTYPE_P(1);
    int i;
    Datum *key_datums;
    bool *key_nulls;
    int elem_count;

    deconstruct_array(keys, TEXTOID, -1, false, 'i', &key_datums, &key_nulls,
                      &elem_count);

    for (i = 0; i < elem_count; i++)
    {
        agtype_value strVal;

        if (key_nulls[i])
        {
            continue;
        }

        strVal.type = AGTV_STRING;
        strVal.val.string.val = VARDATA(key_datums[i]);
        strVal.val.string.len = VARSIZE(key_datums[i]) - VARHDRSZ;

        if (find_agtype_value_from_container(&agt->root,
                                        AGT_FOBJECT | AGT_FARRAY,
                                        &strVal) != NULL)
        {
            PG_RETURN_BOOL(true);
        }
    }

    PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(agtype_exists_all);
/*
 * ?& operator for agtype. Returns true if all of the array strings exist as
 * top-level keys
 */
Datum agtype_exists_all(PG_FUNCTION_ARGS)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    ArrayType  *keys = PG_GETARG_ARRAYTYPE_P(1);
    int i;
    Datum *key_datums;
    bool *key_nulls;
    int elem_count;

    deconstruct_array(keys, TEXTOID, -1, false, 'i', &key_datums, &key_nulls,
                      &elem_count);

    for (i = 0; i < elem_count; i++)
    {
        agtype_value strVal;

        if (key_nulls[i])
        {
            continue;
        }

        strVal.type = AGTV_STRING;
        strVal.val.string.val = VARDATA(key_datums[i]);
        strVal.val.string.len = VARSIZE(key_datums[i]) - VARHDRSZ;

        if (find_agtype_value_from_container(&agt->root,
                                        AGT_FOBJECT | AGT_FARRAY,
                                        &strVal) == NULL)
        {
            PG_RETURN_BOOL(false);
        }
    }

    PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(agtype_concat);

Datum agtype_concat(PG_FUNCTION_ARGS)
{
    agtype *agt_lhs = AG_GET_ARG_AGTYPE_P(0);
    agtype *agt_rhs = AG_GET_ARG_AGTYPE_P(1);

    /*
     * Jsonb returns NULL for PG Null, but not for jsonb's NULL value,
     * so we do the same.
     */
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
    {
        PG_RETURN_NULL();
    }

    AG_RETURN_AGTYPE_P(agtype_concat_impl(agt_lhs, agt_rhs));
}

static agtype *agtype_concat_impl(agtype *agt1, agtype *agt2)
{
    agtype_parse_state *state = NULL;
    agtype_value *res;
    agtype_iterator *it1;
    agtype_iterator *it2;

    /*
     * If one of the agtype is empty, just return the other if it's not scalar
     * and both are of the same kind.  If it's a scalar or they are of
     * different kinds we need to perform the concatenation even if one is
     * empty.
     */
    if (AGT_ROOT_IS_OBJECT(agt1) == AGT_ROOT_IS_OBJECT(agt2))
    {
        if (AGT_ROOT_COUNT(agt1) == 0 && !AGT_ROOT_IS_SCALAR(agt2))
        {
            return agt2;
        }
        else if (AGT_ROOT_COUNT(agt2) == 0 && !AGT_ROOT_IS_SCALAR(agt1))
        {
            return agt1;
        }
    }

    it1 = agtype_iterator_init(&agt1->root);
    it2 = agtype_iterator_init(&agt2->root);

    res = iterator_concat(&it1, &it2, &state);

    Assert(res != NULL);

    return (agtype_value_to_agtype(res));
}

/*
 * Iterate over all agtype objects and merge them into one.
 * The logic of this function copied from the same hstore function,
 * except the case, when it1 & it2 represents jbvObject.
 * In that case we just append the content of it2 to it1 without any
 * verifications.
 */
static agtype_value *iterator_concat(agtype_iterator **it1,
                                     agtype_iterator **it2,
                                     agtype_parse_state **state)
{
    agtype_value v1, v2, *res = NULL;
    agtype_iterator_token r1, r2, rk1, rk2;

    r1 = rk1 = agtype_iterator_next(it1, &v1, false);
    r2 = rk2 = agtype_iterator_next(it2, &v2, false);

    /*
     * Both elements are objects.
     */
    if (rk1 == WAGT_BEGIN_OBJECT && rk2 == WAGT_BEGIN_OBJECT)
    {
        /*
         * Append all tokens from v1 to res, except last WAGT_END_OBJECT
         * (because res will not be finished yet).
         */
        push_agtype_value(state, r1, NULL);

        while ((r1 = agtype_iterator_next(it1, &v1, true)) != WAGT_END_OBJECT)
        {
            Assert(r1 == WAGT_KEY || r1 == WAGT_VALUE);
            push_agtype_value(state, r1, &v1);
        }

        /*
         * Append all tokens from v2 to res, except last WAGT_END_OBJECT
         */
        while ((r2 = agtype_iterator_next(it2, &v2, true)) != WAGT_END_OBJECT)
        {
            Assert(r2 == WAGT_KEY || r2 == WAGT_VALUE);
            push_agtype_value(state, r2, &v2);
        }

        /*
         * Append the last token WAGT_END_OBJECT to complete res
         */
        res = push_agtype_value(state, WAGT_END_OBJECT, NULL);
    }
    /*
     * Both elements are arrays (either can be scalar).
     */
    else if (rk1 == WAGT_BEGIN_ARRAY && rk2 == WAGT_BEGIN_ARRAY)
    {
        push_agtype_value(state, r1, NULL);

        while ((r1 = agtype_iterator_next(it1, &v1, true)) != WAGT_END_ARRAY)
        {
            Assert(r1 == WAGT_ELEM);
            push_agtype_value(state, r1, &v1);
        }

        while ((r2 = agtype_iterator_next(it2, &v2, true)) != WAGT_END_ARRAY)
        {
            Assert(r2 == WAGT_ELEM);
            push_agtype_value(state, r2, &v2);
        }

        res = push_agtype_value(state, WAGT_END_ARRAY, NULL);
    }
    /* have we got array || object or object || array? */
    else if (((rk1 == WAGT_BEGIN_ARRAY && !(*it1)->is_scalar) &&
              rk2 == WAGT_BEGIN_OBJECT) ||
             (rk1 == WAGT_BEGIN_OBJECT &&
              (rk2 == WAGT_BEGIN_ARRAY && !(*it2)->is_scalar)))
    {
        agtype_iterator **it_array = rk1 == WAGT_BEGIN_ARRAY ? it1 : it2;
        agtype_iterator **it_object = rk1 == WAGT_BEGIN_OBJECT ? it1 : it2;

        bool prepend = (rk1 == WAGT_BEGIN_OBJECT);

        push_agtype_value(state, WAGT_BEGIN_ARRAY, NULL);

        if (prepend)
        {
            push_agtype_value(state, WAGT_BEGIN_OBJECT, NULL);

            while ((r1 = agtype_iterator_next(it_object, &v1, true)) !=
                    WAGT_END_OBJECT)
            {
                Assert(r1 == WAGT_KEY || r1 == WAGT_VALUE);
                push_agtype_value(state, r1, &v1);
            }

            push_agtype_value(state, WAGT_END_OBJECT, NULL);

            while ((r2 = agtype_iterator_next(it_array, &v2, true)) !=
                    WAGT_END_ARRAY)
            {
                Assert(r2 == WAGT_ELEM);
                push_agtype_value(state, r2, &v2);
            }

            res = push_agtype_value(state, WAGT_END_ARRAY, NULL);
        }
        else
        {
            while ((r1 = agtype_iterator_next(it_array, &v1, true)) !=
                   WAGT_END_ARRAY)
            {
                Assert(r1 == WAGT_ELEM);
                push_agtype_value(state, r1, &v1);
            }

            push_agtype_value(state, WAGT_BEGIN_OBJECT, NULL);

            while ((r2 = agtype_iterator_next(it_object, &v2, true)) !=
                    WAGT_END_OBJECT)
            {
                Assert(r2 == WAGT_KEY || r2 == WAGT_VALUE);
                push_agtype_value(state, r2,&v2);
            }

            push_agtype_value(state, WAGT_END_OBJECT, NULL);

            res = push_agtype_value(state, WAGT_END_ARRAY, NULL);
        }
    }
    else if (rk1 == WAGT_BEGIN_OBJECT)
    {
        /*
         * We have object || array.
         */
        Assert(rk1 == WAGT_BEGIN_OBJECT);
        Assert(rk2 == WAGT_BEGIN_ARRAY);

        push_agtype_value(state, WAGT_BEGIN_ARRAY, NULL);
        push_agtype_value(state, WAGT_BEGIN_OBJECT, NULL);

        while ((r1 = agtype_iterator_next(it1, &v1, true)) != WAGT_END_OBJECT)
        {
            Assert(r1 == WAGT_KEY || r1 == WAGT_VALUE);
            push_agtype_value(state, r1, &v1);
        }

        push_agtype_value(state, WAGT_END_OBJECT, NULL);

        while ((r2 = agtype_iterator_next(it2, &v2, true)) != WAGT_END_ARRAY)
        {
            if (v2.type < AGTV_VERTEX || v2.type > AGTV_PATH)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                         errmsg("invalid right operand for agtype "
                                "concatenation")));
            }

            Assert(r2 == WAGT_ELEM);

            push_agtype_value(state, r2, &v2);
        }

        res = push_agtype_value(state, WAGT_END_ARRAY, NULL);
    }
    else
    {
        /*
         * We have array || object.
         */
        Assert(rk1 == WAGT_BEGIN_ARRAY);
        Assert(rk2 == WAGT_BEGIN_OBJECT);

        push_agtype_value(state, WAGT_BEGIN_ARRAY, NULL);

        while ((r1 = agtype_iterator_next(it1, &v1, true)) != WAGT_END_ARRAY)
        {
            if (v1.type < AGTV_VERTEX || v1.type > AGTV_PATH)
            {
                ereport(ERROR,
                        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("invalid left operand for agtype "
                               "concatenation")));
            }

            Assert(r1 == WAGT_ELEM);

            push_agtype_value(state, r1, &v1);
        }

        push_agtype_value(state, WAGT_BEGIN_OBJECT, NULL);

        while ((r2 = agtype_iterator_next(it2, &v2, true)) != WAGT_END_OBJECT)
        {
            Assert(r2 == WAGT_KEY || r2 == WAGT_VALUE);
            push_agtype_value(state, r2, &v2);
        }

        push_agtype_value(state, WAGT_END_OBJECT, NULL);

        res = push_agtype_value(state, WAGT_END_ARRAY, NULL);
    }

    return res;
}

/*
 * agtype path extraction operator '#>'. The right operand can
 * either be an array of object keys or array indexes for extracting
 * agtype sub-object or sub-array from the left operand.
 */
PG_FUNCTION_INFO_V1(agtype_extract_path);

Datum agtype_extract_path(PG_FUNCTION_ARGS)
{
    return get_agtype_path_all(fcinfo, false);
}

/*
 * agtype path extraction operator '#>>' that returns the extracted path
 * as text.
 */
PG_FUNCTION_INFO_V1(agtype_extract_path_text);

Datum agtype_extract_path_text(PG_FUNCTION_ARGS)
{
    return get_agtype_path_all(fcinfo, true);
}

static Datum get_agtype_path_all(FunctionCallInfo fcinfo, bool as_text)
{
    agtype *agt = AG_GET_ARG_AGTYPE_P(0);
    agtype *path = AG_GET_ARG_AGTYPE_P(1);
    agtype *res;
    int npath;
    int i;
    bool have_object = false, have_array = false;
    agtype_value *agtvp = NULL;
    agtype_value tv;
    agtype_container *container;

    if (AGT_ROOT_IS_SCALAR(path) || AGT_ROOT_IS_OBJECT(path))
    {
        ereport(ERROR,(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                       errmsg("right operand must be an array")));
    }

    if (AGT_ROOT_IS_SCALAR(agt))
    {
        agt = agtype_value_to_agtype(extract_entity_properties(agt, true));
    }

    npath = AGT_ROOT_COUNT(path);
    container = &agt->root;

    /* Identify whether we have object, array, or scalar at top-level */
    if (AGT_ROOT_IS_OBJECT(agt))
    {
        have_object = true;
    }
    else if (AGT_ROOT_IS_ARRAY(agt) && !AGT_ROOT_IS_SCALAR(agt))
    {
        have_array = true;
    }
    else
    {
        Assert(AGT_ROOT_IS_ARRAY(agt) && AGT_ROOT_IS_SCALAR(agt));

        /* Extract the scalar value */
        if (npath <= 0)
        {
            agtvp = get_ith_agtype_value_from_container(container, 0);
        }
    }

    /*
     * If RHS array is empty, return the entire LHS object/array, based on the
     * assumption that we should not do any field or element extractions. In
     * case of non-scalar, we can just hand back the agtype without much
     * work but for the scalar case, fall through and deal with the value
     * below the loop (This inconsistency arises because there's no easy way to
     * generate an agtype_value directly for root-level containers)
     */
    if (npath <= 0 && agtvp == NULL)
    {
        if (as_text)
        {
            PG_RETURN_TEXT_P(cstring_to_text(agtype_to_cstring(NULL, container,
                                                               VARSIZE(agt))));
        }
        else
        {
            /* not text mode - just hand back the agtype */
            AG_RETURN_AGTYPE_P(agt);
        }
    }

    for (i = 0; i < npath; i++)
    {
        agtype_value *cur_key =
            get_ith_agtype_value_from_container(&path->root, i);

        if (have_object && cur_key->type == AGTV_STRING)
        {
            agtvp = find_agtype_value_from_container(container,
                                                     AGT_FOBJECT,
                                                     cur_key);
        }
        else if (have_array)
        {
            long lindex;
            uint32 index;

            /*
             * for array on LHS, there should be an integer or a
             * valid integer string on RHS
             */
            if (cur_key->type == AGTV_INTEGER)
            {
                lindex = cur_key->val.int_value;
            }
            else if (cur_key->type == AGTV_STRING)
            {
                /*
                 * extract the integer from the string,
                 * if character other than a digit is found, return null
                 */
                char* str = NULL;
                lindex = strtol(cur_key->val.string.val, &str, 10);

                if (strcmp(str, ""))
                {
                    PG_RETURN_NULL();
                }
            }
            else
            {
                PG_RETURN_NULL();
            }

            if (lindex > INT_MAX || lindex < INT_MIN)
            {
                PG_RETURN_NULL();
            }

            if (lindex >= 0)
            {
                index = (uint32) lindex;
            }
            else
            {
                /* Handle negative subscript */
                uint32 nelements;

                /* Container must be an array, but make sure */
                if (!AGTYPE_CONTAINER_IS_ARRAY(container))
                {
                    elog(ERROR, "not an agtype array");
                }

                nelements = AGTYPE_CONTAINER_SIZE(container);

                if (-lindex > nelements)
                {
                    PG_RETURN_NULL();
                }
                else
                {
                    index = nelements + lindex;
                }
            }

            agtvp = get_ith_agtype_value_from_container(container, index);
        }
        else
        {
            PG_RETURN_NULL();
        }

        if (agtvp == NULL)
        {
            PG_RETURN_NULL();
        }
        else if (i == npath - 1)
        {
            break;
        }

        if (agtvp->type == AGTV_BINARY)
        {
            agtype_iterator_token r;
            agtype_iterator *it =
                agtype_iterator_init((agtype_container *)
                                      agtvp->val.binary.data);

            r = agtype_iterator_next(&it, &tv, true);
            container = (agtype_container *) agtvp->val.binary.data;
            have_object = r == WAGT_BEGIN_OBJECT;
            have_array = r == WAGT_BEGIN_ARRAY;
        }
        else
        {
            have_object = agtvp->type == AGTV_OBJECT;
            have_array = agtvp->type == AGTV_ARRAY;
        }
    }

    if (as_text)
    {
        /* special-case output for string and null values */
        if (agtvp->type == AGTV_STRING)
        {
            PG_RETURN_TEXT_P(cstring_to_text_with_len(agtvp->val.string.val,
                                                      agtvp->val.string.len));
        }

        if (agtvp->type == AGTV_NULL)
        {
            PG_RETURN_NULL();
        }
    }

    res = agtype_value_to_agtype(agtvp);

    if (as_text)
    {
        PG_RETURN_TEXT_P(cstring_to_text(agtype_to_cstring(NULL,
                                                           &res->root,
                                                           VARSIZE(res))));
    }
    else
    {
        /* not text mode - just hand back the agtype */
        AG_RETURN_AGTYPE_P(res);
    }
}
