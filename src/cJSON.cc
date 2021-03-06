/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
  Copyright (c) 2009 Dave Gamble

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/
/*
 *     Copyright 2016 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <cJSON_utils.h>
#include <platform/cb_malloc.h>
#include <cctype>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gsl/gsl>
#include <new>

#include "cJSON.h"

static int cJSON_strcasecmp(const char *s1, const char *s2)
{
    if (!s1) {
        return (s1 == s2) ? 0 : 1;
    }
    if (!s2) {
        return 1;
    }
    for (; tolower(*s1) == tolower(*s2); ++s1, ++s2) {
        if (*s1 == 0) {
            return 0;
        }
    }
    return tolower(*(const unsigned char *)s1) - tolower(*(const unsigned char *)s2);
}

static void *cJSON_malloc(size_t sz) {
    auto* ret = cb_malloc(sz);
    if (ret == nullptr) {
        throw std::bad_alloc();
    }

    return ret;
}

static void *cJSON_calloc(size_t nmemb, size_t size) {
    auto* ret = cb_calloc(nmemb, size);
    if (ret == nullptr) {
        throw std::bad_alloc();
    }

    return ret;
}

static void cJSON_free(void *ptr) {
    cb_free(ptr);
}

static char *cJSON_strdup(const char *str) {
    auto* ret = cb_strdup(str);
    if (ret == nullptr) {
        throw std::bad_alloc();
    }
    return ret;
}

/* Internal constructor. */
static cJSON *cJSON_New_Item(void)
{
    return reinterpret_cast<cJSON*>(cJSON_calloc(1, sizeof(cJSON)));
}

/* Delete a cJSON structure. */
void cJSON_Delete(cJSON *c)
{
    cJSON *next;
    while (c) {
        next = c->next;
        if (!(c->type & cJSON_IsReference) && c->child) {
            cJSON_Delete(c->child);
        }
        if (!(c->type & cJSON_IsReference) && c->valuestring) {
            cJSON_free(c->valuestring);
        }
        if (c->string) {
            cJSON_free(c->string);
        }
        cJSON_free(c);
        c = next;
    }
}

/**
 * Parse the input text to generate a number, and populate the result into
 * the provided item.
 *
 * A number is defined in https://tools.ietf.org/html/rfc7159 to look like:
 *
 * number = [ minus ] int [ frac ] [ exp ]
 *      decimal-point = %x2E       ; .
 *      digit1-9 = %x31-39         ; 1-9
 *      e = %x65 / %x45            ; e E
 *      exp = e [ minus / plus ] 1*DIGIT
 *      frac = decimal-point 1*DIGIT
 *      int = zero / ( digit1-9 *DIGIT )
 *      minus = %x2D               ; -
 *      plus = %x2B                ; +
 *      zero = %x30                ; 0
 *
 */
static const char *parse_number(cJSON *item, const char *num)
{
    // Unfortunately std::stoull will parse as many characters as
    // there are digit, and stop at the first non-digit instead of
    // throwing an exception if the number isn't an integral number.
    //
    // Let's start by assuming that it is an integer (as that is what
    // we typically use).
    std::size_t pos;
    item->type = cJSON_Number;
    item->valueint = int64_t(std::stoull(num, &pos));

    // Now it _could_ be that this is a floating point number instead.
    const auto next = num[pos];
    if (next == '.' || next == 'e' || next == 'E') {
        // this is a double!
        item->valuedouble = std::stod(num, &pos);
        item->type = cJSON_Double;
    }

    return num + pos;
}

/* Render the number nicely from the given item into a string. */
static char *print_number(const cJSON *item)
{
    char *str;
    str = reinterpret_cast<char*>(cJSON_malloc(21)); /* 2^64+1 can be represented in 21 chars. */
    sprintf(str, "%" PRId64, item->valueint);
    return str;
}

/* Render the number nicely from the given item into a string. */
static char *print_double(const cJSON *item)
{
    double d = item->valuedouble;
    char *str;
    str = reinterpret_cast<char *>(cJSON_malloc(64)); /* This is a nice tradeoff. */
    if (fabs(floor(d) - d) <= DBL_EPSILON) {
        sprintf(str, "%.0f", d);
    } else if (fabs(d) < 1.0e-6 || fabs(d) > 1.0e9) {
        sprintf(str, "%e", d);
    } else {
        sprintf(str, "%f", d);
    }

    return str;
}

/* Parse the input text into an unescaped cstring, and populate item. */
static const unsigned char firstByteMark[7] = { 0x00, 0x00, 0xC0, 0xE0, 0xF0, 0xF8, 0xFC };
static const char *parse_string(cJSON *item, const char *str)
{
    const char *ptr = str + 1;
    char *ptr2;
    char *out;
    int len = 0;
    unsigned uc;
    if (*str != '\"') {
        return NULL; /* not a string! */
    }

    while (*ptr != '\"' && (unsigned char)*ptr > 31 && ++len) {
        if (*ptr++ == '\\') {
            ptr++; /* Skip escaped quotes. */
        }
    }

    out = reinterpret_cast<char*>(cJSON_malloc(len + 1)); /* This is how long we need for the string, roughly. */
    if (!out) {
        return NULL;
    }

    ptr = str + 1;
    ptr2 = out;
    while (*ptr != '\"' && (unsigned char)*ptr > 31) {
        if (*ptr != '\\') {
            *ptr2++ = *ptr++;
        } else {
            ptr++;
            switch (*ptr) {
            case 'b':
                *ptr2++ = '\b';
                break;
            case 'f':
                *ptr2++ = '\f';
                break;
            case 'n':
                *ptr2++ = '\n';
                break;
            case 'r':
                *ptr2++ = '\r';
                break;
            case 't':
                *ptr2++ = '\t';
                break;
            case 'u': /* transcode utf16 to utf8. DOES NOT SUPPORT SURROGATE PAIRS CORRECTLY. */
                sscanf(ptr + 1, "%4x", &uc); /* get the unicode char. */
                len = 3;
                if (uc < 0x80) {
                    len = 1;
                } else if (uc < 0x800) {
                    len = 2;
                }
                ptr2 += len;

                switch (len) {
                case 3:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                case 2:
                    *--ptr2 = ((uc | 0x80) & 0xBF);
                    uc >>= 6;
                case 1:
                    *--ptr2 = (uc | firstByteMark[len]);
                }
                ptr2 += len;
                ptr += 4;
                break;
            default:
                *ptr2++ = *ptr;
                break;
            }
            ptr++;
        }
    }
    *ptr2 = 0;
    if (*ptr == '\"') {
        ptr++;
    }
    item->valuestring = out;
    item->type = cJSON_String;
    return ptr;
}

/* Render the cstring provided to an escaped version that can be printed. */
static char *print_string_ptr(const char *str)
{
    const char *ptr;
    char *ptr2, *out;
    int len = 0;

    if (!str) {
        return cJSON_strdup("");
    }
    ptr = str;
    while (*ptr && ++len) {
        if ((unsigned char)*ptr < 32 || *ptr == '\"' || *ptr == '\\') {
            len++;
        }
        ptr++;
    }

    out = reinterpret_cast<char*>(cJSON_malloc(len + 3));
    ptr2 = out;
    ptr = str;
    *ptr2++ = '\"';
    while (*ptr) {
        if ((unsigned char)*ptr > 31 && *ptr != '\"' && *ptr != '\\') {
            *ptr2++ = *ptr++;
        } else {
            *ptr2++ = '\\';
            switch (*ptr++) {
            case '\\':
                *ptr2++ = '\\';
                break;
            case '\"':
                *ptr2++ = '\"';
                break;
            case '\b':
                *ptr2++ = 'b';
                break;
            case '\f':
                *ptr2++ = 'f';
                break;
            case '\n':
                *ptr2++ = 'n';
                break;
            case '\r':
                *ptr2++ = 'r';
                break;
            case '\t':
                *ptr2++ = 't';
                break;
            default:
                ptr2--;
                break; /* eviscerate with prejudice. */
            }
        }
    }
    *ptr2++ = '\"';
    *ptr2++ = 0;
    return out;
}
/* Invote print_string_ptr (which is useful) on an item. */
static char *print_string(const cJSON *item)
{
    return print_string_ptr(item->valuestring);
}

/* Predeclare these prototypes. */
static const char *parse_value(cJSON *item, const char *value);
static char *print_value(const cJSON *item, int depth, int fmt);
static const char *parse_array(cJSON *item, const char *value);
static char *print_array(const cJSON *item, int depth, int fmt);
static const char *parse_object(cJSON *item, const char *value);
static char *print_object(const cJSON *item, int depth, int fmt);

/* Utility to jump whitespace and cr/lf */
static const char *skip(const char *in)
{
    if (in != nullptr) {
        while (*in && (unsigned char)*in <= 32) {
            in++;
        }
    }
    return in;
}

/* Parse an object - create a new root, and populate. */
cJSON *cJSON_Parse(const char *value)
{
    unique_cJSON_ptr ret(cJSON_New_Item());
    if (!ret) {
        return nullptr; /* memory fail */
    }

    try {
        if (!parse_value(ret.get(), skip(value))) {
            return nullptr;
        }
    } catch (const std::invalid_argument&) {
        // format error..
        ret.reset();
    }

    return ret.release();
}

/* Render a cJSON item/entity/structure to text. */
char *cJSON_Print(const cJSON *item)
{
    return print_value(item, 0, 1);
}

char *cJSON_PrintUnformatted(const cJSON *item)
{
    return print_value(item, 0, 0);
}

void cJSON_Free(char *ptr)
{
    cJSON_free(ptr);
}

/* Parser core - when encountering text, process appropriately. */
static const char *parse_value(cJSON *item, const char *value)
{
    if (!value) {
        return NULL; /* Fail on null. */
    }
    if (*value == '\"') {
        return parse_string(item, value);
    }
    if (*value == '-' || (*value >= '0' && *value <= '9')) {
        return parse_number(item, value);
    }
    if (*value == '[') {
        return parse_array(item, value);
    }
    if (*value == '{') {
        return parse_object(item, value);
    }
    if (!strncmp(value, "null", 4)) {
        item->type = cJSON_NULL;
        return value + 4;
    }
    if (!strncmp(value, "false", 5)) {
        item->type = cJSON_False;
        return value + 5;
    }
    if (!strncmp(value, "true", 4)) {
        item->type = cJSON_True;
        item->valueint = 1;
        return value + 4;
    }

    return NULL; /* failure. */
}

/* Render a value to text. */
static char *print_value(const cJSON *item, int depth, int fmt)
{
    char *out = 0;
    if (!item) {
        return NULL;
    }
    switch ((item->type) & 255) {
    case cJSON_NULL:
        out = cJSON_strdup("null");
        break;
    case cJSON_False:
        out = cJSON_strdup("false");
        break;
    case cJSON_True:
        out = cJSON_strdup("true");
        break;
    case cJSON_Number:
        out = print_number(item);
        break;
    case cJSON_Double:
        out = print_double(item);
        break;
    case cJSON_String:
        out = print_string(item);
        break;
    case cJSON_Array:
        out = print_array(item, depth, fmt);
        break;
    case cJSON_Object:
        out = print_object(item, depth, fmt);
        break;
    }
    return out;
}

/* Build an array from input text. */
static const char *parse_array(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '[') {
        return NULL; /* not an array! */
    }

    item->type = cJSON_Array;
    value = skip(value + 1);
    if (*value == ']') {
        return value + 1; /* empty array. */
    }

    item->child = child = cJSON_New_Item();
    if (!item->child) {
        return NULL; /* memory fail */
    }
    value = skip(parse_value(child, skip(value))); /* skip any spacing, get the value. */
    if (!value) {
        return NULL;
    }

    while (*value == ',') {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item())) {
            return NULL; /* memory fail */
        }
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_value(child, skip(value + 1)));
        if (!value) {
            return NULL; /* memory fail */
        }
    }

    if (*value == ']') {
        return value + 1; /* end of array */
    }
    return NULL; /* malformed. */
}

/* Render an array to text */
static char *print_array(const cJSON *item, int depth, int fmt)
{
    if (item->child == nullptr) {
        // special case where the object don't have any children
        // 4 == "[\n]\0"
        auto* out = reinterpret_cast<char*>(cJSON_malloc(4 + depth));
        auto* ptr = out;
        *ptr++ = '[';
        if (fmt) {
            *ptr++ = '\n';
        }

        if (fmt) {
            for (int ii = 0; ii < depth - 1; ii++) {
                *ptr++ = '\t';
            }
        }
        *ptr++ = ']';
        *ptr = 0;
        return out;
    }

    char **entries;
    char *out = 0, *ptr, *ret;
    size_t len = 5;
    cJSON *child = item->child;
    int numentries = 0, i = 0, fail = 0;

    /* How many entries in the array? */
    while (child) {
        numentries++, child = child->next;
    }
    /* Allocate an array to hold the values for each */
    entries = reinterpret_cast<char**>(cJSON_calloc(numentries, sizeof(char *)));
    if (!entries) {
        return NULL;
    }
    /* Retrieve all the results: */
    child = item->child;
    while (child && !fail) {
        ret = print_value(child, depth + 1, fmt);
        entries[i++] = ret;
        if (ret) {
            len += (int)strlen(ret) + 2 + (fmt ? 1 : 0);
        } else {
            fail = 1;
        }
        child = child->next;
    }

    /* If we didn't fail, try to malloc the output string */
    if (!fail) {
        out = reinterpret_cast<char*>(cJSON_malloc(len));
    }
    /* If that fails, we fail. */
    if (!out) {
        fail = 1;
    }

    /* Handle failure. */
    if (fail) {
        for (i = 0; i < numentries; i++) {
            if (entries[i]) {
                cJSON_free(entries[i]);
            }
        }
        cJSON_free(entries);
        return NULL;
    }

    /* Compose the output array. */
    *out = '[';
    ptr = out + 1;
    *ptr = 0;
    for (i = 0; i < numentries; i++) {
        strcpy(ptr, entries[i]);
        ptr += strlen(entries[i]);
        if (i != numentries - 1) {
            *ptr++ = ',';
            if (fmt) {
                *ptr++ = ' ';
            }*ptr = 0;
        }
        cJSON_free(entries[i]);
    }
    cJSON_free(entries);
    *ptr++ = ']';
    *ptr++ = 0;
    return out;
}

/* Build an object from the text. */
static const char *parse_object(cJSON *item, const char *value)
{
    cJSON *child;
    if (*value != '{') {
        return NULL; /* not an object! */
    }

    item->type = cJSON_Object;
    value = skip(value + 1);
    if (*value == '}') {
        return value + 1; /* empty array. */
    }

    item->child = child = cJSON_New_Item();
    value = skip(parse_string(child, skip(value)));
    if (!value) {
        return NULL;
    }
    child->string = child->valuestring;
    child->valuestring = 0;
    if (*value != ':') {
        return NULL; /* fail! */
    }
    value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
    if (!value) {
        return NULL;
    }

    while (*value == ',') {
        cJSON *new_item;
        if (!(new_item = cJSON_New_Item())) {
            return NULL; /* memory fail */
        }
        child->next = new_item;
        new_item->prev = child;
        child = new_item;
        value = skip(parse_string(child, skip(value + 1)));
        if (!value) {
            return NULL;
        }
        child->string = child->valuestring;
        child->valuestring = 0;
        if (*value != ':') {
            return NULL; /* fail! */
        }
        value = skip(parse_value(child, skip(value + 1))); /* skip any spacing, get the value. */
        if (!value) {
            return NULL;
        }
    }

    if (*value == '}') {
        return value + 1; /* end of array */
    }

    return NULL; /* malformed. */
}

/* Render an object to text. */
static char *print_object(const cJSON *item, int depth, int fmt)
{
    if (item->child == nullptr) {
        // special case where the object don't have any children
        // 4 == "{\n}\0"
        auto* out = reinterpret_cast<char*>(cJSON_malloc(4 + depth));
        auto* ptr = out;
        *ptr++ = '{';
        if (fmt) {
            *ptr++ = '\n';
        }

        if (fmt) {
            for (int ii = 0; ii < depth - 1; ii++) {
                *ptr++ = '\t';
            }
        }
        *ptr++ = '}';
        *ptr = 0;
        return out;
    }

    char **entries = 0, **names = 0;
    char *out = 0, *ptr, *ret, *str;
    size_t len = 7;
    int i = 0, j;
    cJSON *child = item->child;
    int numentries = 0, fail = 0;
    /* Count the number of entries. */
    while (child) {
        numentries++, child = child->next;
    }
    /* Allocate space for the names and the objects */
    entries = reinterpret_cast<char**>(cJSON_calloc(numentries, sizeof(char *)));
    if (!entries) {
        return NULL;
    }
    names = reinterpret_cast<char**>(cJSON_calloc(numentries, sizeof(char *)));
    if (!names) {
        cJSON_free(entries);
        return NULL;
    }

    /* Collect all the results into our arrays: */
    child = item->child;
    depth++;
    if (fmt) {
        len += depth;
    }
    while (child) {
        names[i] = str = print_string_ptr(child->string);
        entries[i++] = ret = print_value(child, depth, fmt);
        if (str && ret) {
            len += (int)strlen(ret) + (int)strlen(str) + 2 + (fmt ? 2 + depth : 0);
        } else {
            fail = 1;
        }
        child = child->next;
    }

    /* Try to allocate the output string */
    if (!fail) {
        out = reinterpret_cast<char*>(cJSON_malloc(len));
    }
    if (!out) {
        fail = 1;
    }

    /* Handle failure */
    if (fail) {
        for (i = 0; i < numentries; i++) {
            if (names[i]) {
                cJSON_free(names[i]);
            }
            if (entries[i]) {
                cJSON_free(entries[i]);
            }
        }
        cJSON_free(names);
        cJSON_free(entries);
        return NULL;
    }

    /* Compose the output: */
    *out = '{';
    ptr = out + 1;
    if (fmt) {
        *ptr++ = '\n';
    }*ptr = 0;
    for (i = 0; i < numentries; i++) {
        if (fmt) {
            for (j = 0; j < depth; j++) {
                *ptr++ = '\t';
            }
        }
        strcpy(ptr, names[i]);
        ptr += strlen(names[i]);
        *ptr++ = ':';
        if (fmt) {
            *ptr++ = '\t';
        }
        strcpy(ptr, entries[i]);
        ptr += strlen(entries[i]);
        if (i != numentries - 1) {
            *ptr++ = ',';
        }
        if (fmt) {
            *ptr++ = '\n';
        }*ptr = 0;
        cJSON_free(names[i]);
        cJSON_free(entries[i]);
    }

    cJSON_free(names);
    cJSON_free(entries);
    if (fmt) {
        for (i = 0; i < depth - 1; i++) {
            *ptr++ = '\t';
        }
    }
    *ptr++ = '}';
    *ptr++ = 0;
    return out;
}

/* Get Array size/item / object item. */
int cJSON_GetArraySize(cJSON *array)
{
    cJSON *c = array->child;
    int i = 0;
    while (c) {
        i++, c = c->next;
    }
    return i;
}

cJSON *cJSON_GetArrayItem(cJSON *array, int item)
{
    cJSON *c = array->child;
    while (c && item > 0) {
        item--, c = c->next;
    }
    return c;
}

cJSON *cJSON_GetObjectItem(cJSON *object, const char *string)
{
    cJSON *c = object->child;
    while (c && cJSON_strcasecmp(c->string, string)) {
        c = c->next;
    }
    return c;
}

/* Utility for array list handling. */
static void suffix_object(cJSON *prev, cJSON *item)
{
    prev->next = item;
    item->prev = prev;
}

/* Utility for handling references. */
static cJSON *create_reference(cJSON *item)
{
    cJSON *ref = cJSON_New_Item();
    memcpy(ref, item, sizeof(cJSON));
    ref->string = 0;
    ref->type |= cJSON_IsReference;
    ref->next = ref->prev = 0;
    return ref;
}

/* Add item to array/object. */
void cJSON_AddItemToArray(cJSON *array, cJSON *item)
{
    cJSON *c = array->child;
    if (!c) {
        array->child = item;
    } else {
        while (c && c->next) {
            c = c->next;
        }
        suffix_object(c, item);
    }
}

void cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item)
{
    if (item->string) {
        cJSON_free(item->string);
    }
    item->string = cJSON_strdup(string);
    cJSON_AddItemToArray(object, item);
}

void cJSON_AddItemReferenceToArray(cJSON *array, cJSON *item)
{
    cJSON_AddItemToArray(array, create_reference(item));
}

void cJSON_AddItemReferenceToObject(cJSON *object, const char *string, cJSON *item)
{
    cJSON_AddItemToObject(object, string, create_reference(item));
}

cJSON *cJSON_DetachItemFromArray(cJSON *array, int which)
{
    cJSON *c = array->child;
    while (c && which > 0) {
        c = c->next, which--;
    }
    if (!c) {
        return NULL;
    }
    if (c->prev) {
        c->prev->next = c->next;
    }
    if (c->next) {
        c->next->prev = c->prev;
    }
    if (c == array->child) {
        array->child = c->next;
    }
    c->prev = c->next = 0;
    return c;
}

void cJSON_DeleteItemFromArray(cJSON *array, int which)
{
    cJSON_Delete(cJSON_DetachItemFromArray(array, which));
}

cJSON *cJSON_DetachItemFromObject(cJSON *object, const char *string)
{
    int i = 0;
    cJSON *c = object->child;
    while (c && cJSON_strcasecmp(c->string, string)) {
        i++, c = c->next;
    }
    if (c) {
        return cJSON_DetachItemFromArray(object, i);
    }
    return NULL;
}

void cJSON_DeleteItemFromObject(cJSON *object, const char *string)
{
    cJSON_Delete(cJSON_DetachItemFromObject(object, string));
}

/* Replace array/object items with new ones. */
void cJSON_ReplaceItemInArray(cJSON *array, int which, cJSON *newitem)
{
    cJSON *c = array->child;
    while (c && which > 0) {
        c = c->next, which--;
    }
    if (!c) {
        return;
    }
    newitem->next = c->next;
    newitem->prev = c->prev;
    if (newitem->next) {
        newitem->next->prev = newitem;
    }
    if (c == array->child) {
        array->child = newitem;
    } else {
        newitem->prev->next = newitem;
    }
    c->next = c->prev = 0;
    cJSON_Delete(c);
}

void cJSON_ReplaceItemInObject(cJSON *object, const char *string, cJSON *newitem)
{
    int i = 0;
    cJSON *c = object->child;
    while (c && cJSON_strcasecmp(c->string, string)) {
        i++, c = c->next;
    }
    if (c) {
        newitem->string = cJSON_strdup(string);
        cJSON_ReplaceItemInArray(object, i, newitem);
    }
}

/* Create basic types: */
cJSON *cJSON_CreateNull(void)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_NULL;
    return item;
}

cJSON *cJSON_CreateTrue(void)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_True;
    return item;
}

cJSON *cJSON_CreateFalse(void)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_False;
    return item;
}

cJSON *cJSON_CreateNumber(int64_t num)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_Number;
    item->valueint = num;
    return item;
}

cJSON *cJSON_CreateDouble(double num)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_Double;
    item->valuedouble = num;
    return item;
}

cJSON *cJSON_CreateString(const char *string)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_String;
    item->valuestring = cJSON_strdup(string);
    return item;
}

cJSON *cJSON_CreateArray(void)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_Array;
    return item;
}

cJSON *cJSON_CreateObject(void)
{
    cJSON *item = cJSON_New_Item();
    item->type = cJSON_Object;
    return item;
}

void cJSON_AddDoubleToObject(cJSON *object,
                             const char *string,
                             double value) {
    cJSON_AddItemToObject(object, string, cJSON_CreateDouble(value));
}

extern void cJSON_AddBoolToObject(cJSON* object, const char* string, bool value)
{
    if (value) {
        cJSON_AddTrueToObject(object, string);
    } else {
        cJSON_AddFalseToObject(object, string);
    }
}

void cJSON_AddUintPtrToObject(cJSON* obj, const char* name, uintptr_t value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "0x%" PRIxPTR, value);
    cJSON_AddItemToObject(obj, name, cJSON_CreateString(buffer));
}

void cJSON_AddIntegerToObject(cJSON* object, const char* string, uint32_t value){
    cJSON_AddNumberToObject(object, string, value);
}

void cJSON_AddInteger64ToObject(cJSON* object, const char* string, uint64_t value){
    cJSON_AddItemToObject(object, string, cJSON_CreateNumber(int64_t(value)));
}

void cJSON_AddStringifiedIntegerToObject(cJSON* object, const char* string, uint64_t value){
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%" PRIu64, value);
    cJSON_AddItemToObject(object, string, cJSON_CreateString(buffer));
}

extern void cJSON_AddStringifiedSignedIntegerToObject(cJSON* object, const char* string, int64_t value){
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%" PRId64, value);
    cJSON_AddItemToObject(object, string, cJSON_CreateString(buffer));
}
