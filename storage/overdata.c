/*  $Id$
**
**  Overview data processing.
**
**  Here be routines for creating and checking the overview data, the
**  tab-separated list of overview fields.
*/

#include "config.h"
#include "clibrary.h"
#include <ctype.h>

#include "inn/buffer.h"
#include "inn/messages.h"
#include "inn/qio.h"
#include "inn/vector.h"
#include "libinn.h"
#include "macros.h"
#include "ovinterface.h"
#include "paths.h"


/* The standard overview fields. */
static const char * const fields[] = {
    "Subject", "From", "Date", "Message-ID", "References", "Bytes", "Lines"
};


/*
**  Parse the overview schema and return a vector of the additional fields
**  over the standard ones.  Caller is responsible for freeing the vector.
*/
struct vector *
overview_extra_fields(void)
{
    struct vector *list;
    char *schema = NULL;
    char *line, *p;
    QIOSTATE *qp = NULL;
    unsigned int field;
    bool full = false;

    schema = concatpath(innconf->pathetc, _PATH_SCHEMA);
    qp = QIOopen(schema);
    if (qp == NULL) {
        syswarn("cannot open %s", schema);
        goto fail;
    }
    list = vector_new();
    for (field = 0, line = QIOread(qp); line != NULL; line = QIOread(qp)) {
        while (ISWHITE(*line))
            line++;
        p = strchr(line, '#');
        if (p != NULL)
            *p = '\0';
        p = strchr(line, '\n');
        if (p != NULL)
            *p = '\0';
        if (*line == '\0')
            continue;
        p = strchr(line, ':');
        if (p != NULL) {
            *p++ = '\0';
            full = (strcmp(p, "full") == 0);
        }
        if (field >= SIZEOF(fields)) {
            if (!full)
                warn("additional field %s not marked with :full", line);
            vector_add(list, line);
        } else {
            if (strcasecmp(line, fields[field]) != 0)
                warn("field %d is %s, should be %s", field, line,
                     fields[field]);
        }
        field++;
    }
    if (QIOerror(qp)) {
        if (QIOtoolong(qp)) {
            warn("line too long in %s", schema);
        } else {
            syswarn("error while reading %s", schema);
        }
    }
    return list;

 fail:
    if (schema != NULL)
        free(schema);
    if (qp != NULL)
        QIOclose(qp);
    return NULL;
}


/*
**  Given an article, its length, the name of a header, and a buffer to append
**  the data to, append header data for that header to the overview data
**  that's being constructed.  Doesn't append any data if the header isn't
**  found.
*/
static void
build_header(const char *article, size_t length, const char *header,
             struct buffer *overview)
{
    ptrdiff_t size;
    size_t offset;
    const char *data, *end, *p;

    data = HeaderFindMem(article, length, header, strlen(header));
    if (data == NULL)
        return;
    end = strchr(data, '\n');
    while (end != NULL && ISWHITE(end[1]))
        end = strchr(end + 1, '\n');
    if (end == NULL)
        return;

    size = end - data + 1;
    offset = overview->used + overview->left;
    buffer_resize(overview, offset + size);

    for (p = data; p <= end; p++) {
        if (*p == '\r' && p[1] == '\n') {
            p++;
            continue;
        }
        if (*p == '\0' || *p == '\t' || *p == '\n' || *p == '\r')
            overview->data[offset++] = ' ';
        else
            overview->data[offset++] = *p;
        overview->left++;
    }
}


/*
**  Given an article and a vector of additional headers, generate overview
**  data into the provided buffer.  If the buffer parameter is NULL, a new
**  buffer is allocated.  This data doesn't include the article number.  The
**  article should be in wire format.  Returns the buffer containing the
**  overview data.
*/
struct buffer *
overview_build(ARTNUM number, const char *article, size_t length,
               struct vector *extra, struct buffer *overview)
{
    unsigned int field;
    char buffer[32];

    snprintf(buffer, sizeof(buffer), "%lu", number);
    if (overview == NULL)
        overview = buffer_new();
    buffer_set(overview, buffer, strlen(buffer));
    for (field = 0; field < SIZEOF(fields); field++) {
        buffer_append(overview, "\t", 1);
        if (field == 5) {
            snprintf(buffer, sizeof(buffer), "%lu", length);
            buffer_append(overview, buffer, strlen(buffer));
        } else
            build_header(article, length, fields[field], overview);
    }
    for (field = 0; field < extra->count; field++) {
        buffer_append(overview, "\t", 1);
        buffer_append(overview, extra->strings[field],
                      strlen(extra->strings[field]));
        buffer_append(overview, ": ", 2);
        build_header(article, length, extra->strings[field], overview);
    }
    buffer_append(overview, "\r\n", 2);
    return overview;
}


/*
**  Check whether a given string is a valid number.
*/
static bool
valid_number(const char *string)
{
    const char *p;

    for (p = string; *p != '\0'; p++)
        if (!CTYPE(isdigit, *p))
            return false;
    return true;
}


/*
**  Check whether a given string is a valid overview string (doesn't contain
**  CR or LF, and if the second argument is true must be preceeded by a header
**  name, colon, and space).  Allow CRLF at the end of the data, but don't
**  require it.
*/
static bool
valid_overview_string(const char *string, bool full)
{
    const unsigned char *p;

    /* RFC 2822 says that header fields must consist of printable ASCII
       characters (characters between 33 and 126, inclusive) excluding colon.
       We also allow high-bit characters, just in case, but not DEL. */
    p = (const unsigned char *) string;
    if (full) {
        for (; *p != '\0' && *p != ':'; p++)
            if (*p < 33 || *p == 127)
                return false;
        if (*p != ':')
            return false;
        p++;
        if (*p != ' ')
            return false;
    }
    for (p++; *p != '\0'; p++) {
        if (*p == '\015' && p[1] == '\012' && p[2] == '\0')
            break;
        if (*p == '\015' || *p == '\012')
            return false;
    }
    return true;
}


/*
**  Check the given overview data and make sure it's well-formed.  Extension
**  headers are not checked against overview.fmt (having a different set of
**  extension headers doesn't make the data invalid), but the presence of the
**  standard fields is checked.  Also checked is whether the article number in
**  the data matches the passed article number.  Returns true if the data is
**  okay, false otherwise.
*/
bool
overview_check(const char *data, size_t length, ARTNUM article)
{
    char *copy;
    struct cvector *overview;
    ARTNUM overnum;
    size_t i;

    copy = xstrndup(data, length);
    overview = cvector_split(copy, '\t', NULL);

    /* The actual checks.  We don't verify all of the data, since that data
       may be malformed in the article, but we do check to be sure that the
       fields that should be numbers are numbers.  That should catch most
       positional errors.  We can't check Lines yet since right now INN is
       still accepting the value from the post verbatim. */
    if (overview->count < 8)
        goto fail;
    if (!valid_number(overview->strings[0]))
        goto fail;
    overnum = strtoul(overview->strings[0], NULL, 10);
    if (overnum != article)
        goto fail;
    if (!valid_number(overview->strings[6]))
        goto fail;
    for (i = 1; i < 6; i++)
        if (!valid_overview_string(overview->strings[i], false))
            goto fail;
    for (i = 8; i < overview->count; i++)
        if (!valid_overview_string(overview->strings[i], true))
            goto fail;
    cvector_free(overview);
    free(copy);
    return true;

 fail:
    cvector_free(overview);
    free(copy);
    return false;
}
