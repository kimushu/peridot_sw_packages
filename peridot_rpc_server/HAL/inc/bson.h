#ifndef __BSON_H__
#define __BSON_H__

#ifdef __cplusplus
extern "C" {
#endif

extern const char bson_empty_document[];
extern const int bson_empty_size;
extern const int bson_first_offset;

extern int bson_get_props(const void *doc, ...);
extern const char *bson_get_string(const void *doc, int offset, const char *default_value);
extern void *bson_get_subdocument(void *doc, int offset, void *default_value);
extern const void *bson_get_binary(const void *doc, int offset, int *lenptr);
extern int bson_get_int32(const void *doc, int offset, int default_value);

extern int bson_set_string(void *doc, const char *key, const char *string);
extern int bson_measure_string(const char *key, const char *string);
extern int bson_set_subdocument(void *doc, const char *key, const void *sub_doc);
extern int bson_measure_subdocument(const char *key, const void *doc);
extern int bson_set_array(void *doc, const char *key, const void *sub_doc);
extern int bson_measure_array(const char *key, const void *doc);
extern int bson_set_int32(void *doc, const char *key, int value);
extern int bson_measure_int32(const char *key);
extern int bson_set_null(void *doc, const char *key);
extern int bson_measure_null(const char *key);
extern int bson_set_binary_generic(void *doc, const char* key, int binlen, void **bufptr);
extern int bson_measure_binary(const char *key, int binlen);
extern int bson_set_element(void *doc1, const char *key, const void *doc2, int offset);
extern int bson_measure_element(const char *key, const void *doc, int offset);
extern int bson_measure_document(const void *doc);

#ifdef __cplusplus
}	/* extern "C" */
#endif

#endif  /* __BSON_H__ */
