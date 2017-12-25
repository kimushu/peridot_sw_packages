#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

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
	int len = 0;

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

static const void *seek_data(const void *doc, int offset, unsigned short types, const char **pend)
{
	const char *data;
	const char *end;

	if ((!doc) || (offset < 4)) {
		// No BSON
		return NULL;
	}

	// Check BSON document structure
	end = (const char *)doc + read_unaligned_int(doc);
	data = (const char *)doc + offset;
	if ((end <= data) || (end[-1] != 0x00)) {
		// Not valid BSON
		return NULL;
	}

	// Validate element type
	if ((data[0] != (types & 0xff)) && (data[0] != (types >> 8))) {
		// Type error
		return NULL;
	}
	++data;

	// Skip element name
	data += strnlen(data, end - data) + 1;
	if ((end <= data) || (data[-1] != '\0')) {
		// Not valid BSON
		return NULL;
	}

	if (pend) {
		*pend = end;
	}
	return data;
}

const char *bson_get_string(const void *doc, int offset, const char *default_value)
{
	const char *data;
	const char *end;
	int len;

	data = seek_data(doc, offset, 0x0202, &end);
	if (!data) {
		return default_value;
	}

	// Read length (includes NUL byte)
	len = read_unaligned_int(data);
	data += 4;
	if ((len <= 0) || ((end - data) < len) || (data[len - 1] != '\0')) {
		return default_value;
	}
	return data;
}

void *bson_get_subdocument(void *doc, int offset, void *default_value)
{
	char *data = (char *)seek_data(doc, offset, 0x0304, NULL);
	if (!data) {
		return default_value;
	}
	return data;
}

const void *bson_get_binary(const void *doc, int offset, int *lenptr)
{
	const char *data;
	const char *end;
	int binlen;

	data = seek_data(doc, offset, 0x0505, &end);
	if (!data) {
		return NULL;
	}

	// Read binary length and skip subtype
	binlen = read_unaligned_int(data);
	data += (4 + 1);
	if ((binlen <= 0) || (end - data) < binlen) {
		return NULL;
	}

	if (lenptr) {
		*lenptr = binlen;
	}

	return data;
}

int bson_get_boolean(const void *doc, int offset, int default_value)
{
	const char *data = seek_data(doc, offset, 0x0808, NULL);
	if (!data) {
		return default_value;
	}
	return data[0] ? 1 : 0;
}

int bson_get_int32(const void *doc, int offset, int default_value)
{
	const char *data = seek_data(doc, offset, 0x1010, NULL);
	if (!data) {
		return default_value;
	}
	return read_unaligned_int(data);
}

double bson_get_double(const void *doc, int offset, double default_value)
{
	union {
		double d;
		char c[8];
	} result;
	const char *data = seek_data(doc, offset, 0x0101, NULL);
	if (!data) {
		return default_value;
	}
	memcpy(result.c, data, 8);
	return result.d;
}

void bson_create_empty_document(void *doc)
{
	((unsigned char *)doc)[0] = 5;
	((unsigned char *)doc)[1] = 0;
	((unsigned char *)doc)[2] = 0;
	((unsigned char *)doc)[3] = 0;
	((unsigned char *)doc)[4] = 0;
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

static int set_subelement(void *doc, const char *key, char type, const void *ptr, int len)
{
	int keylen = strlen(key) + 1;

	if (doc) {
		char *data = (char *)doc;
		char *end = data + read_unaligned_int(data);

		end[-1] = type;
		memcpy(end, key, keylen);
		end += keylen;

		memcpy(end, ptr, len);
		end += len;

		*end++ = 0x00;
		write_unaligned_int(data, end - data);
	}

	return 1 + keylen + len;
}

static int set_subdocument(void *doc, const char *key, char type, const void *sub_doc)
{
	return set_subelement(doc, key, type, sub_doc, read_unaligned_int(sub_doc));
}

int bson_set_subdocument(void *doc, const char *key, const void *sub_doc)
{
	return set_subdocument(doc, key, 0x03, sub_doc);
}

int bson_set_array(void *doc, const char *key, const void *sub_doc)
{
	return set_subdocument(doc, key, 0x04, sub_doc);
}

int bson_set_boolean(void *doc, const char *key, int value)
{
	char bool_value = (value ? 1 : 0);
	return set_subelement(doc, key, 0x08, &bool_value, 1);
}

int bson_set_int32(void *doc, const char *key, int value)
{
	return set_subelement(doc, key, 0x10, &value, 4);
}

int bson_set_double(void *doc, const char *key, double value)
{
	return set_subelement(doc, key, 0x01, &value, 8);
}

int bson_set_null(void *doc, const char *key)
{
	return set_subelement(doc, key, 0x0a, NULL, 0);
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

int bson_shrink_binary(void *doc, void *buf, int binlen)
{
	char *lenptr = (char *)buf - 5;
	int old_binlen = read_unaligned_int(lenptr);
	int old_doclen;

	if (((char *)buf)[old_binlen] != 0x00) {
		// Shrink is allowed only for the last element
		return -1;
	}
	if (old_binlen < binlen) {
		// Growth is not allowed
		return -1;
	}
	if (old_binlen == binlen) {
		// Nothing to do
		return 0;
	}

	// Set new end marker and binary length
	((char *)buf)[binlen] = 0x00;
	write_unaligned_int(lenptr, binlen);

	// Adjust entire document length
	old_doclen = read_unaligned_int(doc);
	write_unaligned_int(doc, old_doclen - (old_binlen - binlen));
	return 0;
}

int bson_set_element(void *doc1, const char *key, const void *doc2, int offset)
{
	const char *data2;
	const char *end2;
	char type;
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

	return set_subelement(doc1, key, type, data2, elmlen);

no_bson:
not_valid_bson:
	return 0;
}

int bson_measure_document(const void *doc)
{
	return read_unaligned_int(doc);
}

void *bson_alloc(int content_length)
{
	void *doc = malloc(5 /* bson_empty_size */ + content_length);
	if (doc) {
		bson_create_empty_document(doc);
	}
	return doc;
}

void bson_free(void *doc)
{
	free(doc);
}

#ifdef BSON_ENABLE_DUMP
#include <stdio.h>
#include <inttypes.h>

static const void *bson_dump_impl(const void *doc, int indent, void (*output)(void *user, const char *fmt, ...), void *user, int is_array, int total_indent)
{
	const char *data;
	const char *end;
	char *indent_text;
	const char *sep = "";

	if (!doc) {
		(*output)(user, "null");
		return NULL;
	}

	data = (const char *)doc;
	end = data + read_unaligned_int(data);
	data += 4;

	if (indent) {
		total_indent += indent;
		indent_text = (char *)alloca(total_indent + 2);
		memset(indent_text + 1, ' ', total_indent);
		indent_text[0] = '\n';
		indent_text[total_indent] = '\0';
	} else {
		indent_text = (char *)"";
	}
	(*output)(user, "%c", is_array ? '[' : '{');

	while (data < end) {
		const char *key;
		char type = *data++;
		if (type == 0) {
			(*output)(user, "\n%c" + (indent ? 0 : 1), is_array ? ']' : '}');
			if (data < end) {
				(*output)(user, "!!! Junk data after document !!!");
			}
			return end;
		}

		key = data;
		data += (strlen(key) + 1);
		if (data >= end) {
			(*output)(user, "!!! Key is too long !!!");
			return NULL;
		}
		(*output)(user, "%s%s\"%s\":", sep, indent_text, key);
		sep = ",";

		switch (type) {
		case 0x01:
			// 64-bit binary floating point
			{
				double value;
				memcpy(&value, data, 8);
				data += 8;
				(*output)(user, "%lf", value);
			}
			break;
		case 0x02:
			// UTF-8 string
			{
				int len = 0;
				const char *value;
				memcpy(&len, data, 4);
				data += 4;
				value = data;
				data += len;
				(*output)(user, "\"%s\"", value);
				if (data[-1] != '\0') {
					(*output)(user, "!!! Invalid NUL byte !!!");
					return NULL;
				}
			}
			break;
		case 0x03:
		case 0x04:
			// Embedded document / Array
			data = (const char *)bson_dump_impl(data, indent, output, user, (type == 4), total_indent);
			if (!data) {
				return data;
			}
			break;
		case 0x05:
			// Binary data
			{
				int len = 0;
				int off;
				const unsigned char *value;
				unsigned char subtype;
				memcpy(&len, data, 4);
				data += 4;
				subtype = *data++;
				value = (const unsigned char *)data;
				data += len;
				(*output)(user, "<Binary");
				switch (subtype) {
				case 0x00:
					break;
				case 0x01:
					(*output)(user, ":Function");
					break;
				case 0x04:
					(*output)(user, ":UUID");
					break;
				case 0x05:
					(*output)(user, ":MD5");
					break;
				case 0x80:
					(*output)(user, ":User");
					break;
				default:
					(*output)(user, ":0x%02x", subtype);
					break;
				}
				for (off = 0; off < len; ++off) {
					(*output)(user, " %02x", *value++);
				}
				(*output)(user, ">");
			}
			break;
		case 0x06:
			// Undefined value
			(*output)(user, "undefined");
			break;
		case 0x08:
			// Boolean
			(*output)(user, *data++ ? "true" : "false");
			break;
		case 0x09:
		case 0x12:
			// UTC datetime / 64-bit integer
			{
				long long value;
				memcpy(&value, data, 8);
				data += 8;
				(*output)(user, "%"PRId64, value);
			}
			break;
		case 0x0a:
			// Null value
			(*output)(user, "null");
			break;
		case 0x10:
			// 32-bit integer
			{
				long value;
				memcpy(&value, data, 4);
				data += 4;
				(*output)(user, "%ld", value);
			}
			break;
		case 0x11:
			// Timestamp
			{
				unsigned long long value;
				memcpy(&value, data, 8);
				data += 8;
				(*output)(user, "%"PRIu64, value);
			}
			break;
		default:
			(*output)(user, "!!! Unsupported type: 0x%02x !!!", type);
			return NULL;
		}
	}

	(*output)(user, "!!! Invalid EOD !!!");
	return NULL;
}

typedef struct {
	int length;
	int capacity;
	char *buffer;
} bson_dump_block;

static void bson_dump_append(bson_dump_block *block, const char *fmt, ...)
{
	char temp[128];
	int growth;
	int new_len;
	int capacity;
	va_list args;

	va_start(args, fmt);
	growth = vsnprintf(temp, sizeof(temp), fmt, args);
	va_end(args);

	new_len = block->length + growth;
	capacity = block->capacity;
	if (new_len >= capacity) {
		if (capacity == 0) {
			capacity = sizeof(temp);
		}
		while (new_len >= capacity) {
			capacity *= 2;
		}
		char *new_buffer = realloc(block->buffer, capacity);
		if (!new_buffer) {
			return;
		}
		block->buffer = new_buffer;
		block->capacity = capacity;
	}
	memcpy(block->buffer + block->length, temp, growth + 1);
	block->length = new_len;
}

char *bson_dump(const void *doc, int indent)
{
	bson_dump_block block = { 0 };
	bson_dump_impl(doc, indent, (void (*)(void *, const char *, ...))bson_dump_append, &block, 0, 0);
	return block.buffer;
}
#endif	/* BSON_ENABLE_DUMP */
