// include/pdftext.h — Nyx OS PDF Text Extractor
// Extracts readable text from simple (uncompressed) PDF files
// stored in the Nyx filesystem. Handles PDF-1.x with basic fonts
// and uncompressed content streams.
#ifndef PDFTEXT_H
#define PDFTEXT_H

// Maximum number of indirect objects we can track
#define PDF_MAX_OBJECTS 32

// Each indirect object record
typedef struct {
    int  obj_num;       // object number
    int  gen_num;       // generation number
    int  offset;        // byte offset of "N G obj"
    int  length;        // estimated length (to next obj or endobj)
} PdfObject;

// Parsed PDF state
typedef struct {
    const char* data;           // raw PDF content
    int          size;          // content length
    PdfObject    objects[PDF_MAX_OBJECTS];
    int          object_count;
    int          root_obj;      // object number of catalog
    int          pages_obj;     // object number of page tree root
} PdfDoc;

// Extract text from a PDF content buffer, writing into output (up to max_out
// bytes). Returns the number of characters written, or -1 if the data does
// not appear to be a valid PDF.
int pdftext_extract(const char* pdf_data, int data_size, char* output, int max_out);

// Lightweight page counter — returns the number of pages in the PDF, or -1
// on error. Does NOT extract text, so it's fast even on larger files.
int pdftext_get_page_count(const char* pdf_data, int data_size);

#endif
