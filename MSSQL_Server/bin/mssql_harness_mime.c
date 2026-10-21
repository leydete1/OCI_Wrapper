#include <string.h>
#include <strings.h>
const char *get_mime_type(const char *filename);
const char *get_mime_type(const char *filename)
{
    const char *ext = strrchr(filename, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "bmp") == 0) return "image/bmp";
    if (strcasecmp(ext, "pdf") == 0) return "application/pdf";
    return "application/octet-stream";
}
