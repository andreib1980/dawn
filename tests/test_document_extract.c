/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * By contributing to this project, you agree to license your contributions
 * under the GPLv3 (or any later version) or any future licenses chosen by
 * the project author(s). Contributions include any modifications,
 * enhancements, or additions to the project. These contributions become
 * part of the project and are adopted by the project author(s).
 *
 * Unit tests for document_extract.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tools/document_extract.h"
#include "unity.h"

void setUp(void) {
}
void tearDown(void) {
}

/* =============================================================================
 * Extension Validation Tests
 * ============================================================================= */

static void test_extension_allowed(void) {
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".txt"), ".txt allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".md"), ".md allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".csv"), ".csv allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".json"), ".json allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".c"), ".c allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".h"), ".h allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".py"), ".py allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".js"), ".js allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".html"), ".html allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".htm"), ".htm allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".xml"), ".xml allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".yaml"), ".yaml allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".yml"), ".yml allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".toml"), ".toml allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".css"), ".css allowed");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".sql"), ".sql allowed");

   /* Case insensitive */
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".TXT"), ".TXT case insensitive");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".Md"), ".Md case insensitive");
   TEST_ASSERT_TRUE_MESSAGE(document_extension_allowed(".JSON"), ".JSON case insensitive");

   /* Disallowed */
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".exe"), ".exe not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".bin"), ".bin not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".zip"), ".zip not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".tar"), ".tar not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".dll"), ".dll not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(".so"), ".so not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(""), "empty not allowed");
   TEST_ASSERT_FALSE_MESSAGE(document_extension_allowed(NULL), "NULL not allowed");
}

/* =============================================================================
 * Extension From Filename Tests
 * ============================================================================= */

static void test_get_extension(void) {
   TEST_ASSERT_EQUAL_STRING(".txt", document_get_extension("file.txt"));
   TEST_ASSERT_EQUAL_STRING(".pdf", document_get_extension("my.doc.pdf"));
   TEST_ASSERT_EQUAL_STRING(".DOCX", document_get_extension("report.DOCX"));
   TEST_ASSERT_NULL_MESSAGE(document_get_extension("noext"), "noext -> NULL");
   TEST_ASSERT_NULL_MESSAGE(document_get_extension(NULL), "NULL -> NULL");
}

/* =============================================================================
 * Content-Type Mapping Tests
 * ============================================================================= */

static void test_content_type_mapping(void) {
   TEST_ASSERT_EQUAL_STRING(".pdf", document_extension_from_content_type("application/pdf"));
   TEST_ASSERT_EQUAL_STRING(".html", document_extension_from_content_type("text/html"));
   TEST_ASSERT_EQUAL_STRING(".html",
                            document_extension_from_content_type("text/html; charset=utf-8"));
   TEST_ASSERT_EQUAL_STRING(".txt", document_extension_from_content_type("text/plain"));
   TEST_ASSERT_EQUAL_STRING(".json", document_extension_from_content_type("application/json"));
   TEST_ASSERT_EQUAL_STRING(".md", document_extension_from_content_type("text/markdown"));
   TEST_ASSERT_EQUAL_STRING(".csv", document_extension_from_content_type("text/csv"));
   TEST_ASSERT_EQUAL_STRING(".xml", document_extension_from_content_type("text/xml"));
   TEST_ASSERT_EQUAL_STRING(".xml", document_extension_from_content_type("application/xml"));
   TEST_ASSERT_EQUAL_STRING(".html", document_extension_from_content_type("application/xhtml+xml"));
   TEST_ASSERT_EQUAL_STRING(".yaml", document_extension_from_content_type("text/yaml"));

   const char *docx_ct = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
   TEST_ASSERT_EQUAL_STRING(".docx", document_extension_from_content_type(docx_ct));

   /* Unknown types */
   TEST_ASSERT_NULL_MESSAGE(document_extension_from_content_type("application/octet-stream"),
                            "octet-stream -> NULL");
   TEST_ASSERT_NULL_MESSAGE(document_extension_from_content_type("image/png"), "image/png -> NULL");
   TEST_ASSERT_NULL_MESSAGE(document_extension_from_content_type(NULL), "NULL -> NULL");
   TEST_ASSERT_NULL_MESSAGE(document_extension_from_content_type(""), "empty -> NULL");

   /* Partial match should not match */
   TEST_ASSERT_NULL_MESSAGE(document_extension_from_content_type("text/htmlx"),
                            "text/htmlx should not match text/html");
}

/* =============================================================================
 * Plain Text Extraction Tests
 * ============================================================================= */

static void test_plain_text_extraction(void) {
   const char *text = "Hello, this is a test document.\nLine two.";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(text, strlen(text), ".txt", 1024 * 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_SUCCESS, rc, "plain text extraction succeeds");
   TEST_ASSERT_NOT_NULL_MESSAGE(result.text, "result text not NULL");
   TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(text), (int)result.text_len, "text length matches");
   TEST_ASSERT_EQUAL_STRING_MESSAGE(text, result.text, "text content matches");
   TEST_ASSERT_EQUAL_INT_MESSAGE(-1, result.page_count, "page_count is -1 for non-PDF");
   free(result.text);
}

static void test_plain_text_truncation(void) {
   const char *text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(text, strlen(text), ".txt", 10, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_SUCCESS, rc, "truncated extraction succeeds");
   TEST_ASSERT_EQUAL_INT_MESSAGE(10, (int)result.text_len, "truncated to max_extracted_size");
   TEST_ASSERT_EQUAL_MEMORY("ABCDEFGHIJ", result.text, 10);
   free(result.text);
}

static void test_code_file_extraction(void) {
   const char *code = "#include <stdio.h>\nint main() { return 0; }\n";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(code, strlen(code), ".c", 1024 * 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_SUCCESS, rc, ".c extraction succeeds");
   TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(code), (int)result.text_len, ".c text length matches");
   free(result.text);

   memset(&result, 0, sizeof(result));
   rc = document_extract_from_buffer(code, strlen(code), ".py", 1024 * 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_SUCCESS, rc, ".py extraction succeeds");
   free(result.text);
}

/* =============================================================================
 * Error Case Tests
 * ============================================================================= */

static void test_empty_input(void) {
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(NULL, 0, ".txt", 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_EMPTY, rc, "NULL data returns EMPTY");

   rc = document_extract_from_buffer("test", 0, ".txt", 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_EMPTY, rc, "zero-length returns EMPTY");
}

static void test_unsupported_extension(void) {
   const char *data = "some data";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(data, strlen(data), ".exe", 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_UNSUPPORTED, rc, ".exe returns UNSUPPORTED");

   rc = document_extract_from_buffer(data, strlen(data), ".bin", 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_UNSUPPORTED, rc, ".bin returns UNSUPPORTED");
}

static void test_invalid_pdf_magic(void) {
#ifdef HAVE_MUPDF
   const char *not_pdf = "This is not a PDF file at all";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(not_pdf, strlen(not_pdf), ".pdf", 1024 * 1024, 100,
                                         &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_CORRUPT, rc,
                                 "non-PDF data with .pdf ext returns CORRUPT");
   TEST_ASSERT_NULL_MESSAGE(result.text, "no text on corrupt PDF");
#else
   TEST_IGNORE_MESSAGE("MuPDF not available — skipping PDF tests");
#endif
}

static void test_invalid_docx_magic(void) {
#if defined(HAVE_LIBZIP) && defined(HAVE_LIBXML2)
   const char *not_docx = "This is definitely not a DOCX/ZIP file";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(not_docx, strlen(not_docx), ".docx", 1024 * 1024, 100,
                                         &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_ERROR_CORRUPT, rc,
                                 "non-ZIP data with .docx ext returns CORRUPT");
   TEST_ASSERT_NULL_MESSAGE(result.text, "no text on corrupt DOCX");
#else
   TEST_IGNORE_MESSAGE("libzip/libxml2 not available — skipping DOCX tests");
#endif
}

/* =============================================================================
 * Error String Tests
 * ============================================================================= */

static void test_error_strings(void) {
   TEST_ASSERT_EQUAL_STRING("Success", document_extract_error_string(DOC_EXTRACT_SUCCESS));
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_UNSUPPORTED)) >
                                0,
                            "UNSUPPORTED has message");
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_CORRUPT)) > 0,
                            "CORRUPT has message");
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_EMPTY)) > 0,
                            "EMPTY has message");
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_TOO_LARGE)) > 0,
                            "TOO_LARGE has message");
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(DOC_EXTRACT_ERROR_ALLOC)) > 0,
                            "ALLOC has message");
   TEST_ASSERT_TRUE_MESSAGE(strlen(document_extract_error_string(999)) > 0,
                            "unknown code has message");
}

/* =============================================================================
 * HTML Extraction Tests
 * ============================================================================= */

static void test_html_extraction(void) {
   const char *html = "<html><body><h1>Title</h1><p>Hello world</p></body></html>";
   doc_extract_result_t result = { 0 };

   int rc = document_extract_from_buffer(html, strlen(html), ".html", 1024 * 1024, 100, &result);
   TEST_ASSERT_EQUAL_INT_MESSAGE(DOC_EXTRACT_SUCCESS, rc, "HTML extraction succeeds");
   TEST_ASSERT_NOT_NULL_MESSAGE(result.text, "HTML extracted text not NULL");
   TEST_ASSERT_TRUE_MESSAGE(result.text_len > 0, "HTML extracted text has content");
   /* Should contain "Title" and "Hello world" without tags */
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.text, "Title"), "HTML text contains 'Title'");
   TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.text, "Hello world"),
                                "HTML text contains 'Hello world'");
   TEST_ASSERT_NULL_MESSAGE(strstr(result.text, "<h1>"), "HTML tags stripped");
   free(result.text);
}

/* =============================================================================
 * Main
 * ============================================================================= */

int main(void) {
   UNITY_BEGIN();
   RUN_TEST(test_extension_allowed);
   RUN_TEST(test_get_extension);
   RUN_TEST(test_content_type_mapping);
   RUN_TEST(test_plain_text_extraction);
   RUN_TEST(test_plain_text_truncation);
   RUN_TEST(test_code_file_extraction);
   RUN_TEST(test_empty_input);
   RUN_TEST(test_unsupported_extension);
   RUN_TEST(test_invalid_pdf_magic);
   RUN_TEST(test_invalid_docx_magic);
   RUN_TEST(test_error_strings);
   RUN_TEST(test_html_extraction);
   return UNITY_END();
}
