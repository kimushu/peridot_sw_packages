#include <stdarg.h>
#include <stddef.h>
#include <string.h>

const char bson_empty_document[] = {5,0,0,0,0};
const int bson_empty_size = sizeof(bson_empty_document);
const int bson_first_offset = 4;

static int read_unaligned_int(const void *ptr)
{
	const unsigned char *bytes = (const unsigned char *)ptr;
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24);
}

static void write_unaligned_int(void *ptr, int len)
{
	unsigned char *bytes = (unsigned char *)ptr;
	bytes[0] = (len >>  0) & 0xff;
	bytes[1] = (len >>  8) & 0xff;
	bytes[2] = (len >> 16) & 0xff;
	bytes[3] = (len >> 24) & 0xff;
}

static int measure_value(char type, const char *data, const char *end)
{
	const char *start = data;
	int len;

	switch (type) {
	case 0x01:	// 64-bit floating point
	case 0x09:	// UTC timedate
	case 0x11:	// Timestamp
	case 0x12:	// 64-bit integer
		len = 8;
		break;
	case 0x02:	// UTF-8 string
	case 0x0d:	// JavaScript code
	case 0x0e:	// Symbol (Deprecated)
	case 0x0f:	// JavaScript code w/ scope
		len = read_unaligned_int(data);
		data += 4;
		break;
	case 0x03:	// Embedded document
	case 0x04:	// Array
		len = read_unaligned_int(data);
		break;
	case 0x05:	// Binary
		len = read_unaligned_int(data);
		data += (4 + 1);
		break;
	case 0x06:	// undefined (Deprecated)
	case 0x0a:	// null
		len = 0;
		break;
	case 0x07:	// ObjectId
		len = 12;
		break;
	case 0x08:	// Boolean
		len = 1;
		break;
	case 0x0b:	// Regular expression
		data += strnlen(data, end - data) + 1;
		if ((end <= data) || (data[-1] != '\0')) {
			goto not_valid_bson;
		}
		data += strnlen(data, end - data) + 1;
		if ((end <= data) || (data[-1] != '\0')) {
			goto not_valid_bson;
		}
		break;
	case 0x0c:	// DB pointer (Deprecated)
		len = read_unaligned_int(data);
		data += (4 + 12);
		break;
	case 0x10:	// 32-bit integer
		len = 4;
		break;
	case 0x13:	// 128-bit floating point
		len = 16;
		break;
	default:	// Unknown type
		goto not_valid_bson;
	}

	if (len < 0) {
		goto not_valid_bson;
	}
	return len + (data - start);

not_valid_bson:
	return -1;
}

int bson_get_props(const void *doc, ...)
{
	const char *data;
	const char *end;
	va_list keys;
	int total;
	int scanned;

	// Check BSON document structure
	end = (const char *)doc + read_unaligned_int(doc);
	data = (const char *)doc + 4;
	if ((end <= data) || (end[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Clear all param offsets
	va_start(keys, doc);
	total = 0;
	for (;;) {
		const char *key = va_arg(keys, const char *);
		if (!key) {
			break;
		}
		*va_arg(keys, int *) = -1;
		++total;
	}
	va_end(keys);

	// Scan elements
	scanned = 0;
	while (data < end) {
		char type = *data++;
		const char *name = data;
		int elmlen;

		if (type == 0x00) {
			// End of element list
			break;
		}

		data += strnlen(name, end - name) + 1;
		if ((end <= data) || (data[-1] != '\0')) {
			goto not_valid_bson;
		}

		va_start(keys, doc);
		for (;;) {
			int *off;
			const char *key = va_arg(keys, const char *);
			if (!key) {
				break;
			}
			off = va_arg(keys, int *);
			if ((*off < 0) && (strcmp(key, name) == 0)) {
				*off = name - 1 - (const char *)doc;
				if (++scanned == total) {
					// No more elements to scan
					return scanned;
				}
				// To next element
				break;
			}
		}
		va_end(keys);

		elmlen = measure_value(type, data, end);
		if (elmlen < 0) {
			goto not_valid_bson;
		}
		data += elmlen;
	}

	return scanned;

not_valid_bson:
	return -1;
}

const char *bson_get_string(const void *doc, int offset, const char *default_value)
{
	const char *data;
	const char *end;
	int len;

	if ((!doc) || (offset < 4)) {
		goto no_bson;
	}

	// Check BSON document structure
	end = (const char *)doc + read_unaligned_int(doc);
	data = (const char *)doc + offset;
	if ((end <= data) || (end[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Validate element type
	if (data[0] != 0x02) {
		goto type_error;
	}
	++data;

	// Skip element name
	data += strnlen(data, end - data) + 1;
	if ((end <= data) || (data[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Read length (includes NUL byte)
	len = read_unaligned_int(data);
	data += 4;
	if ((len <= 0) || (end - data) < len) {
		goto not_valid_bson;
	}

	// Check NUL byte
	if (data[len - 1] != '\0') {
		goto not_valid_bson;
	}

	return data;

no_bson:
not_valid_bson:
type_error:
	return default_value;
}

void *bson_get_subdocument(void *doc, int offset, void *default_value)
{
	char *data;
	char *end;

	if ((!doc) || (offset < 4)) {
		goto no_bson;
	}

	// Check BSON document structure
	end = (char *)doc + read_unaligned_int(doc);
	data = (char *)doc + offset;
	if ((end <= data) || (end[-1] != '\0')) {
		goto not_valid_bson;
	}

	switch (*data++) {
	case 0x03:	// Embedded document
	case 0x04:	// Array
		break;
	default:
		goto type_error;
	}

	data += strnlen(data, end - data) + 1;
	if ((end <= data) || (data[-1] != '\0')) {
		goto not_valid_bson;
	}

	return data;

no_bson:
not_valid_bson:
type_error:
	return default_value;
}

const void *bson_get_binary(const void *doc, int offset, int *lenptr)
{
	const char *data;
	const char *end;
	int binlen;

	if ((!doc) || (offset < 4)) {
		goto no_bson;
	}

	// Check BSON document structure
	end = (const char *)doc + read_unaligned_int(doc);
	data = (const char *)doc + offset;
	if ((end <= data) || (end[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Validate element type
	if (data[0] != 0x05) {
		goto type_error;
	}
	++data;

	// Skip element name
	data += strnlen(data, end - data) + 1;
	if ((end <= data) || (data[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Read binary length and skip subtype
	binlen = read_unaligned_int(data);
	data += (4 + 1);
	if ((binlen <= 0) || (end - data) < binlen) {
		goto not_valid_bson;
	}

	if (lenptr) {
		*lenptr = binlen;
	}

	return data;

no_bson:
not_valid_bson:
type_error:
	return NULL;
}

int bson_get_int32(const void *doc, int offset, int default_value)
{
	const char *data;
	const char *end;

	if ((!doc) || (offset < 4)) {
		goto no_bson;
	}

	// Check BSON document structure
	end = (const char *)doc + read_unaligned_int(doc);
	data = (const char *)doc + offset;
	if ((end <= data) || (end[-1] != '\0')) {
		goto not_valid_bson;
	}

	// Validate element type
	if (data[0] != 0x10) {
		goto type_error;
	}
	++data;

	// Skip element name
	data += strnlen(data, end - data) + 1;
	if ((end <= data) || (data[-1] != '\0')) {
		goto not_valid_bson;
	}

	return read_unaligned_int(data);

no_bson:
not_valid_bson:
type_error:
	return default_value;
}

int bson_set_string(void *doc, const char *key, const char *string)
{
	int keylen = strlen(key) + 1;
	int datalen = strlen(string) + 1;

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = 0x02;
		memcpy(end, key, keylen);
		end += keylen;

		write_unaligned_int(end, datalen);
		end += 4;

		memcpy(end, string, datalen);
		end += datalen;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen + 4 + datalen;
}

int bson_measure_string(const char *key, const char *string)
{
	return bson_set_string(NULL, key, string);
}

static int set_subdocument(void *doc, char type, const char *key, const void *sub_doc)
{
	int keylen = strlen(key) + 1;
	int sublen = read_unaligned_int(sub_doc);

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = type;
		memcpy(end, key, keylen);
		end += keylen;

		memcpy(end, sub_doc, sublen);
		end += sublen;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen + sublen;
}

int bson_set_subdocument(void *doc, const char *key, const void *sub_doc)
{
	return set_subdocument(doc, 0x03, key, sub_doc);
}

int bson_measure_subdocument(const char *key, const void *doc)
{
	return set_subdocument(NULL, 0x03, key, doc);
}

int bson_set_array(void *doc, const char *key, const void *sub_doc)
{
	return set_subdocument(doc, 0x04, key, sub_doc);
}

int bson_measure_array(const char *key, const void *doc)
{
	return set_subdocument(NULL, 0x04, key, doc);
}

int bson_set_int32(void *doc, const char *key, int value)
{
	int keylen = strlen(key) + 1;

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = 0x10;
		memcpy(end, key, keylen);
		end += keylen;

		memcpy(end, &value, 4);
		end += 4;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen + 4;
}

int bson_measure_int32(const char *key)
{
	return bson_set_int32(NULL, key, 0);
}

int bson_set_null(void *doc, const char *key)
{
	int keylen = strlen(key) + 1;

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = 0x0a;
		memcpy(end, key, keylen);
		end += keylen;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen;
}

int bson_measure_null(const char *key)
{
	return bson_set_null(NULL, key);
}

static int bson_set_binary(void *doc, const char *key, int binlen, void **bufptr, char subtype)
{
	int keylen = strlen(key) + 1;

	if (binlen < 0) {
		return -1;
	}

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = 0x05;
		memcpy(end, key, keylen);
		end += keylen;

		write_unaligned_int(end, binlen);
		end += 4;

		*end++ = subtype;

		if (bufptr) {
			*bufptr = end;
		}
		memset(end, 0, binlen);
		end += binlen;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen + 4 + 1 + binlen;
}

int bson_set_binary_generic(void *doc, const char* key, int binlen, void **bufptr)
{
	return bson_set_binary(doc, key, binlen, bufptr, 0x00);
}

int bson_measure_binary(const char *key, int binlen)
{
	return bson_set_binary_generic(NULL, key, binlen, NULL);
}

int bson_set_element(void *doc1, const char *key, const void *doc2, int offset)
{
	const char *data2;
	const char *end2;
	char type;
	int keylen;
	int elmlen;

	if ((!doc2) || (offset < 4)) {
		goto no_bson;
	}

	end2 = (const char *)doc2 + read_unaligned_int(doc2);
	data2 = (const char *)doc2 + offset;
	if ((end2 <= (data2 + 1)) || (end2[-1] != '\0')) {
		goto not_valid_bson;
	}

	type = *data2++;

	data2 += strnlen(data2, end2 - data2) + 1;
	if ((end2 <= data2) || (data2[-1] != '\0')) {
		goto not_valid_bson;
	}

	elmlen = measure_value(type, data2, end2);
	if (elmlen < 0) {
		goto not_valid_bson;
	}

	keylen = strlen(key) + 1;

	if (doc1) {
		char *data1 = (char *)doc1;
		char *end1 = data1 + read_unaligned_int(data1);

		end1[-1] = type;
		memcpy(end1, key, keylen);
		end1 += keylen;
		memcpy(end1, data2, elmlen);
		end1 += elmlen;
		
		*end1++ = 0x00;
		write_unaligned_int(doc1, end1 - data1);
	}

	return 1 + keylen + elmlen;

no_bson:
not_valid_bson:
	return 0;
}

int bson_measure_element(const char *key, const void *doc, int offset)
{
	return bson_set_element(NULL, key, doc, offset);
}

int bson_measure_document(const void *doc)
{
	return read_unaligned_int(doc);
}

