/* Squizzy Studio platform - minimal XML DOM (parse + write). Adequate for config, skins, archives.
   Model: element tree; each element has name, ordered attributes, children and a text value
   (concatenated character data + CDATA directly inside it). Comments, PIs and DOCTYPE are
   skipped on parse. Whitespace-only text in elements with children is dropped. UTF-8 only.
   Nodes are owned by their document; pointers stay valid until removed or the doc is freed.
   Not thread-safe per document. */
#ifndef SQ_PLATFORM_XML_H
#define SQ_PLATFORM_XML_H

#include "sq/platform/api.h"

typedef struct sq_xml_doc sq_xml_doc;
typedef struct sq_xml_node sq_xml_node;

/* err (optional) receives "line N: message". */
SQP_API sq_result sq_xml_parse(const char* data, size_t len, sq_xml_doc** out, char* err, size_t err_size);
SQP_API sq_result sq_xml_load_file(const char* path, sq_xml_doc** out, char* err, size_t err_size);
SQP_API sq_xml_doc* sq_xml_create(const char* root_name);
SQP_API void sq_xml_free(sq_xml_doc* doc);
SQP_API sq_xml_node* sq_xml_root(sq_xml_doc* doc);
/* Serializes with an <?xml ...?> header. pretty: 2-space indentation. */
SQP_API sq_result sq_xml_write(const sq_xml_doc* doc, sq_bool pretty, sq_buffer* out);
SQP_API sq_result sq_xml_save_file(const sq_xml_doc* doc, const char* path, sq_bool pretty);

SQP_API const char* sq_xml_name(const sq_xml_node* n);
SQP_API sq_result sq_xml_set_name(sq_xml_node* n, const char* name);
SQP_API const char* sq_xml_text(const sq_xml_node* n); /* never NULL */
SQP_API sq_result sq_xml_set_text(sq_xml_node* n, const char* text);

SQP_API uint32_t sq_xml_attr_count(const sq_xml_node* n);
SQP_API const char* sq_xml_attr_name(const sq_xml_node* n, uint32_t i);
SQP_API const char* sq_xml_attr_value(const sq_xml_node* n, uint32_t i);
SQP_API const char* sq_xml_attr(const sq_xml_node* n, const char* name); /* NULL if missing */
SQP_API sq_result sq_xml_set_attr(sq_xml_node* n, const char* name, const char* value);
SQP_API sq_result sq_xml_remove_attr(sq_xml_node* n, const char* name);

/* name == NULL matches any element. */
SQP_API sq_xml_node* sq_xml_parent(const sq_xml_node* n);
SQP_API sq_xml_node* sq_xml_first_child(const sq_xml_node* n, const char* name);
SQP_API sq_xml_node* sq_xml_next_sibling(const sq_xml_node* n, const char* name);
SQP_API uint32_t sq_xml_child_count(const sq_xml_node* n);
SQP_API sq_xml_node* sq_xml_append_child(sq_xml_node* parent, const char* name);
SQP_API sq_result sq_xml_remove_child(sq_xml_node* parent, sq_xml_node* child); /* frees child subtree */

#endif
