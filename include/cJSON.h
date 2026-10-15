/*
  Copyright (c) 2009-2017 Dave Gamble and cJSON contributors
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

#ifndef cJSON__h
#define cJSON__h

#ifdef __cplusplus
extern "C"
{
#endif

#include <stddef.h>

/* cJSON Types: */
#define cJSON_Invalid (0)
#define cJSON_False  (1 << 0)
#define cJSON_True   (1 << 1)
#define cJSON_NULL   (1 << 2)
#define cJSON_Number (1 << 3)
#define cJSON_String (1 << 4)
#define cJSON_Array  (1 << 5)
#define cJSON_Object (1 << 6)
#define cJSON_Raw    (1 << 7)

#define cJSON_IsReference 256
#define cJSON_StringIsConst 512

/* The cJSON structure: */
typedef struct cJSON
{
    struct cJSON *next;
    struct cJSON *prev;
    struct cJSON *child;
    int type;
    char *valuestring;
    int valueint;
    double valuedouble;
    char *string;
} cJSON;

typedef struct cJSON_Hooks
{
    void *(*malloc_fn)(size_t sz);
    void (*free_fn)(void *ptr);
} cJSON_Hooks;

/* Supply malloc, realloc and free functions to cJSON */
extern void cJSON_InitHooks(cJSON_Hooks* hooks);

/* Memory Management */
extern void cJSON_free(void *object);
extern char* cJSON_strdup(const char* string);

/* Parse/Print */
extern cJSON *cJSON_Parse(const char *value);
extern char  *cJSON_Print(const cJSON *item);
extern char  *cJSON_PrintUnformatted(const cJSON *item);
extern void   cJSON_Delete(cJSON *c);

/* Array/Object manipulation */
extern int cJSON_GetArraySize(const cJSON *array);
extern cJSON *cJSON_GetArrayItem(const cJSON *array, int index);
extern cJSON *cJSON_GetObjectItem(const cJSON *object, const char *string);
extern cJSON *cJSON_GetObjectItemCaseSensitive(const cJSON *object, const char *string);
extern int cJSON_HasObjectItem(const cJSON *object, const char *string);

/* Create items */
extern cJSON *cJSON_CreateNull(void);
extern cJSON *cJSON_CreateTrue(void);
extern cJSON *cJSON_CreateFalse(void);
extern cJSON *cJSON_CreateBool(int b);
extern cJSON *cJSON_CreateNumber(double num);
extern cJSON *cJSON_CreateString(const char *string);
extern cJSON *cJSON_CreateArray(void);
extern cJSON *cJSON_CreateObject(void);

/* Add items to array/object */
extern cJSON *cJSON_AddItemToArray(cJSON *array, cJSON *item);
extern cJSON *cJSON_AddItemToObject(cJSON *object, const char *string, cJSON *item);
extern cJSON *cJSON_AddNullToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddTrueToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddFalseToObject(cJSON * const object, const char * const name);
extern cJSON *cJSON_AddBoolToObject(cJSON * const object, const char * const name, int boolean);
extern cJSON *cJSON_AddNumberToObject(cJSON * const object, const char * const name, double number);
extern cJSON *cJSON_AddStringToObject(cJSON * const object, const char * const name, const char * const string);

/* Helper for checking type */
#define cJSON_IsFalse(item) ((item) && ((item)->type & cJSON_False))
#define cJSON_IsTrue(item) ((item) && ((item)->type & cJSON_True))
#define cJSON_IsBool(item) ((item) && (((item)->type & cJSON_True) || ((item)->type & cJSON_False)))
#define cJSON_IsNull(item) ((item) && ((item)->type & cJSON_NULL))
#define cJSON_IsNumber(item) ((item) && ((item)->type & cJSON_Number))
#define cJSON_IsString(item) ((item) && ((item)->type & cJSON_String))
#define cJSON_IsArray(item) ((item) && ((item)->type & cJSON_Array))
#define cJSON_IsObject(item) ((item) && ((item)->type & cJSON_Object))

/* Duplicate */
extern cJSON *cJSON_Duplicate(const cJSON *item, int recurse);

#ifdef __cplusplus
}
#endif

#endif
