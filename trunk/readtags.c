/*
*   $Id$
*
*   Copyright (c) 1996-2001, Darren Hiebert
*
*   This source code is released into the public domain.
*
*   This module contains functions for reading tag files.
*/

/*
*   INCLUDE FILES
*/
#include "general.h"	/* must always come first */

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>	/* declare off_t */
#endif
#include <stdio.h>
#include "readtags.h"

/*
*   MACROS
*/
#define TAB '\t'


/*
*   DATA DECLARATIONS
*/
typedef struct {
    size_t size;
    char *buffer;
} vstring;

/* Information about current tag file */
struct sTagFile {
	/* has the file been opened and this structure initialized? */
    short initialized;
	/* format of tag file */
    short format;
	/* is the tag file sorted? */
    short sorted;
	/* pointer to file structure */
    FILE* fp;
	/* file position of first character of `line' */
    off_t pos;
	/* size of tag file in seekable positions */
    off_t size;
	/* last line read */
    vstring line;
	/* name of tag in last line read */
    vstring name;
	/* defines tag search state */
    struct {
		/* file position of last match for tag */
	    off_t pos; 
		/* name of tag last searched for */
	    const char *name;
		/* length of name for partial matches */
	    size_t nameLength;
		/* peforming partial match */
	    short partial;
		/* ignoring case */
	    short ignorecase;
    } search;
	/* miscellaneous extension fields */
    struct {
		/* number of entries in `list' */
	    unsigned short max;
		/* list of key value pairs */
	    tagExtensionField *list;
    } fields;
	/* buffers to be freed at close */
    struct {
	    /* name of program author */
	char *author;
	    /* name of program */
	char *name;
	    /* URL of distribution */
	char *url;
	    /* program version */
	char *version;
    } program;
};

/*
*   DATA DEFINITIONS
*/
const char *const EmptyString = "";
const char *const PseudoTagPrefix = "!_";

/*
*   FUNCTION DEFINITIONS
*/

static int growString (vstring *s)
{
    int result = 0;
    size_t newLength;
    char *newLine;
    if (s->size == 0)
    {
	newLength = 128;
	newLine = (char*) malloc (newLength);
	*newLine = '\0';
    }
    else
    {
	newLength = 2 * s->size;
	newLine = (char*) realloc (s->buffer, newLength);
    }
    if (newLine == NULL)
	perror ("string too large");
    else
    {
	s->buffer = newLine;
	s->size = newLength;
	result = 1;
    }
    return result;
}

/* Copy name of tag out of tag line */
static void copyName (tagFile *const file)
{
    size_t length;
    const char *end = strchr (file->line.buffer, '\t');
    if (end == NULL)
    {
	end = strchr (file->line.buffer, '\n');
	if (end == NULL)
	    end = strchr (file->line.buffer, '\r');
    }
    if (end != NULL)
	length = end - file->line.buffer;
    else
	length = strlen (file->line.buffer);
    while (length >= file->name.size)
	growString (&file->name);
    strncpy (file->name.buffer, file->line.buffer, length);
    file->name.buffer [length] = '\0';
}

static int readTagLineRaw (tagFile *const file)
{
    int result = 1;
    int reReadLine;

    /*  If reading the line places any character other than a null or a
     *  newline at the last character position in the buffer (one less than
     *  the buffer size), then we must resize the buffer and reattempt to read
     *  the line.
     */
    do
    {
	char *const pLastChar = file->line.buffer + file->line.size - 2;
	char *line;

	file->pos = ftell (file->fp);
	reReadLine = 0;
	*pLastChar = '\0';
	line = fgets (file->line.buffer, (int) file->line.size, file->fp);
	if (line == NULL)
	{
	    /* read error */
	    if (! feof (file->fp))
		perror ("readTagLine");
	    result = 0;
	}
	else if (*pLastChar != '\0'  &&
		    *pLastChar != '\n'  &&  *pLastChar != '\r')
	{
	    /*  buffer overflow */
	    growString (&file->line);
	    fseek (file->fp, file->pos, SEEK_SET);
	    reReadLine = 1;
	}
	else
	{
	    size_t i = strlen (file->line.buffer);
	    while (i > 0  &&
		   (file->line.buffer [i - 1] == '\n' || file->line.buffer [i - 1] == '\r'))
	    {
		file->line.buffer [i - 1] = '\0';
		--i;
	    }
	}
    } while (reReadLine  &&  result);
    if (result)
	copyName (file);
    return result;
}

static int readTagLine (tagFile *const file)
{
    int result;
    do
    {
	result = readTagLineRaw (file);
    } while (result && *file->name.buffer == '\0');
    return result;
}

static tagResult growFields (tagFile *const file)
{
    tagResult result = TagFailure;
    size_t newCount = 2 * file->fields.max;
    tagExtensionField *newFields = (tagExtensionField*)
	    realloc (file->fields.list, newCount * sizeof (tagExtensionField));
    if (newFields == NULL)
	perror ("too many extension fields");
    else
    {
	file->fields.list = newFields;
	file->fields.max = newCount;
	result = TagSuccess;
    }
    return result;
}

static void parseExtensionFields (tagFile *const file, tagEntry *const entry,
				  char *const string)
{
    char *p = string;
    while (p != NULL  &&  *p != '\0')
    {
	while (*p == TAB)
	    *p++ = '\0';
	if (*p != '\0')
	{
	    char *colon;
	    char *field = p;
	    p = strchr (p, TAB);
	    if (p != NULL)
		*p++ = '\0';
	    colon = strchr (field, ':');
	    if (colon == NULL)
		entry->kind = field;
	    else
	    {
		const char *key = field;
		const char *value = colon + 1;
		*colon = '\0';
		if (strcmp (key, "kind") == 0)
		    entry->kind = value;
		else if (strcmp (key, "file") == 0)
		    entry->fileScope = 1;
		else if (strcmp (key, "line") == 0)
		    entry->address.lineNumber = atol (value);
		else
		{
		    if (entry->fields.count == file->fields.max)
			growFields (file);
		    file->fields.list [entry->fields.count].key = key;
		    file->fields.list [entry->fields.count].value = value;
		    ++entry->fields.count;
		}
	    }
	}
    }
}

static void parseTagLine (tagFile *file, tagEntry *const entry)
{
    int i;
    char *p = file->line.buffer;
    char *tab = strchr (p, TAB);
    int fieldsPresent = 0;

    entry->fields.list = NULL;
    entry->fields.count = 0;
    entry->kind = NULL;
    entry->fileScope = 0;

    entry->name = p;
    if (tab != NULL)
    {
	*tab = '\0';
	p = tab + 1;
	entry->file = p;
	tab = strchr (p, TAB);
	if (tab != NULL)
	{
	    *tab = '\0';
	    p = tab + 1;
	    if (*p == '/'  ||  *p == '?')
	    {
		/* parse pattern */
		int delimiter = *(unsigned char*) p;
		entry->address.lineNumber = 0;
		entry->address.pattern = p;
		do
		{
		    p = strchr (p + 1, delimiter);
		} while (p != NULL  &&  *(p - 1) == '\\');
		if (p == NULL)
		{
		    /* invalid pattern */
		}
		else
		    ++p;
	    }
	    else if (isdigit ((int) *(unsigned char*) p))
	    {
		/* parse line number */
		entry->address.pattern = p;
		entry->address.lineNumber = atol (p);
		while (isdigit ((int) *(unsigned char*) p))
		    ++p;
	    }
	    else
	    {
		/* invalid pattern */
	    }
	    fieldsPresent = (strncmp (p, ";\"", 2) == 0);
	    *p = '\0';
	    if (fieldsPresent)
		parseExtensionFields (file, entry, p + 2);
	}
    }
    if (entry->fields.count > 0)
	entry->fields.list = file->fields.list;
    for (i = entry->fields.count  ;  i < file->fields.max  ;  ++i)
    {
	file->fields.list [i].key = NULL;
	file->fields.list [i].value = NULL;
    }
}

static char *duplicate (const char *str)
{
    char *result = NULL;
    if (str != NULL)
    {
	result = (char*) malloc (strlen (str) + 1);
	if (result == NULL)
	    perror (NULL);
	else
	    strcpy (result, str);
    }
    return result;
}

static void readPseudoTags (tagFile *const file, tagFileInfo *const info)
{
    fpos_t startOfLine;
    size_t prefixLength = strlen (PseudoTagPrefix);
    if (info != NULL)
    {
	info->file.format = 1;
	info->file.sorted = 0;
	info->program.author  = NULL;
	info->program.name    = NULL;
	info->program.url     = NULL;
	info->program.version = NULL;
    }
    while (1)
    {
	fgetpos (file->fp, &startOfLine);
	if (! readTagLine (file))
	    break;
	if (strncmp (file->line.buffer, PseudoTagPrefix, prefixLength) != 0)
	    break;
	else
	{
	    tagEntry entry;
	    const char *key, *value;
	    parseTagLine (file, &entry);
	    key = entry.name + prefixLength;
	    value = entry.file;
	    if (strcmp (key, "TAG_FILE_SORTED") == 0)
		file->sorted = atoi (value);
	    else if (strcmp (key, "TAG_FILE_FORMAT") == 0)
		file->format = atoi (value);
	    else if (strcmp (key, "TAG_PROGRAM_AUTHOR") == 0)
		file->program.author = duplicate (value);
	    else if (strcmp (key, "TAG_PROGRAM_NAME") == 0)
		file->program.name = duplicate (value);
	    else if (strcmp (key, "TAG_PROGRAM_URL") == 0)
		file->program.url = duplicate (value);
	    else if (strcmp (key, "TAG_PROGRAM_VERSION") == 0)
		file->program.version = duplicate (value);
	    if (info != NULL)
	    {
		info->file.format     = file->format;
		info->file.sorted     = file->sorted;
		info->program.author  = file->program.author;
		info->program.name    = file->program.name;
		info->program.url     = file->program.url;
		info->program.version = file->program.version;
	    }
	}
    }
    fsetpos (file->fp, &startOfLine);
}

static tagFile *initialize (const char *const filePath, tagFileInfo *const info)
{
    tagFile *const result = (tagFile*) malloc (sizeof (tagFile));
    if (result == NULL)
	perror ("Cannot open tag file");
    else
    {
	memset (result, 0, sizeof (tagFile));
	growString (&result->line);
	growString (&result->name);
	result->fields.max = 20;
	result->fields.list = (tagExtensionField*) malloc (
	    result->fields.max * sizeof (tagExtensionField));
	result->fp = fopen (filePath, "r");
	if (result->fp == NULL)
	    perror ("cannot open tag file");
	else
	{
	    fseek (result->fp, 0, SEEK_END);
	    result->size = ftell (result->fp);
	    rewind (result->fp);
	    readPseudoTags (result, info);
	    result->initialized = 1;
	}
    }
    return result;
}

static void terminate (tagFile *const file)
{
    fclose (file->fp);

    free (file->line.buffer);
    free (file->name.buffer);
    free (file->fields.list);

    if (file->program.author != NULL)
	free (file->program.author);
    if (file->program.name != NULL)
	free (file->program.name);
    if (file->program.url != NULL)
	free (file->program.url);
    if (file->program.version != NULL)
	free (file->program.version);

    memset (file, 0, sizeof (tagFile));

    free (file);
}

static tagResult readNext (tagFile *const file, tagEntry *const entry)
{
    tagResult result = TagFailure;
    if (file == NULL  ||  ! file->initialized)
	result = TagFailure;
    else if (! readTagLine (file))
	result = TagFailure;
    else
    {
	if (entry != NULL)
	    parseTagLine (file, entry);
	result = TagSuccess;
    }
    return result;
}

static const char *readFieldValue (
    const tagEntry *const entry, const char *const key)
{
    const char *result = NULL;
    int i;
    if (strcmp (key, "kind") == 0)
	result = entry->kind;
    else if (strcmp (key, "file") == 0)
	result = EmptyString;
    else for (i = 0  ;  i < entry->fields.count  &&  result != NULL  ;  ++i)
	if (strcmp (entry->fields.list [i].key, key) == 0)
	    result = entry->fields.list [i].value;
    return result;
}

static int readTagLineSeek (tagFile *const file, const off_t pos)
{
    int result = 0;
    if (fseek (file->fp, pos, SEEK_SET) == 0)
    {
	result = readTagLine (file);		/* read probable partial line */
	if (pos > 0  &&  result)
	    result = readTagLine (file);	/* read complete line */
    }
    return result;
}

#if !defined (HAVE_STRCASECMP) && !defined (HAVE_STRICMP)
static int strcasecmp (const char *s1, const char *s2)
{
    int result;
    do
    {
	result = toupper ((int) *s1) - toupper ((int) *s2);
    } while (result == 0  &&  *s1++ != '\0'  &&  *s2++ != '\0');
    return result;
}
#endif

#if !defined (HAVE_STRNCASECMP) && !defined (HAVE_STRNICMP)
static int strncasecmp (const char *s1, const char *s2, size_t n)
{
    int result;
    do
    {
	result = toupper ((int) *s1) - toupper ((int) *s2);
    } while (result == 0  &&  --n > 0  &&  *s1++ != '\0'  &&  *s2++ != '\0');
    return result;
}
#endif

static int nameComparison (tagFile *const file)
{
    int result;
    if (file->search.ignorecase)
    {
	if (file->search.partial)
	    result = strncasecmp (file->search.name, file->name.buffer,
			       file->search.nameLength);
	else
	    result = strcasecmp (file->search.name, file->name.buffer);
    }
    else
    {
	if (file->search.partial)
	    result = strncmp (file->search.name, file->name.buffer,
			      file->search.nameLength);
	else
	    result = strcmp (file->search.name, file->name.buffer);
    }
    return result;
}

static void findFirstNonMatchBefore (tagFile *const file)
{
#define JUMP_BACK 512
    int more_lines;
    int comp;
    off_t start = file->pos;
    off_t pos = start;
    do
    {
	if (pos < (off_t) JUMP_BACK)
	    pos = 0;
	else
	    pos = pos - JUMP_BACK;
	more_lines = readTagLineSeek (file, pos);
	comp = nameComparison (file);
    } while (more_lines  &&  comp == 0  &&  pos > 0  &&  pos < start);
}

static tagResult findFirstMatchBefore (tagFile *const file)
{
    tagResult result = TagFailure;
    int more_lines;
    off_t start = file->pos;
    findFirstNonMatchBefore (file);
    do
    {
	more_lines = readTagLine (file);
	if (nameComparison (file) == 0)
	    result = TagSuccess;
    } while (more_lines  &&  result != TagSuccess  &&  file->pos < start);
    return result;
}

static tagResult findBinary (tagFile *const file)
{
    tagResult result = TagFailure;
    off_t lower_limit = 0;
    off_t upper_limit = file->size;
    off_t last_pos = 0;
    off_t pos = upper_limit / 2;
    while (result != TagSuccess)
    {
	if (! readTagLineSeek (file, pos))
	{
	    /* in case we fell off end of file */
	    result = findFirstMatchBefore (file);
	    break;
	}
	else if (pos == last_pos)
	{
	    /* prevent infinite loop if we backed up to beginning of file */
	    break;
	}
	else
	{
	    const int comp = nameComparison (file);
	    last_pos = pos;
	    if (comp < 0)
	    {
		upper_limit = pos;
		pos = lower_limit + ((upper_limit - lower_limit) / 2);
	    }
	    else if (comp > 0)
	    {
		lower_limit = pos;
		pos = lower_limit + ((upper_limit - lower_limit) / 2);
	    }
	    else if (pos == 0)
		result = TagSuccess;
	    else
		result = findFirstMatchBefore (file);
	}
    }
    return result;
}

static tagResult findSequential (tagFile *const file)
{
    tagResult result = TagFailure;
    if (file->initialized)
    {
	while (result == TagFailure  &&  readTagLine (file))
	{
	    if (nameComparison (file) == 0)
		result = TagSuccess;
	}
    }
    return result;
}

static tagResult find (tagFile *const file, tagEntry *const entry,
		       const char *const name, const int options)
{
    tagResult result = TagFailure;
    file->search.name = name;
    file->search.nameLength = strlen (name);
    file->search.partial = (options & TAG_PARTIALMATCH) != 0;
    file->search.ignorecase = (options & TAG_IGNORECASE) != 0;
    fseek (file->fp, 0, SEEK_END);
    file->size = ftell (file->fp);
    rewind (file->fp);
    if (file->sorted && ! file->search.ignorecase)
	result = findBinary (file);
    else
	result = findSequential (file);

    if (result != TagSuccess)
	file->search.pos = file->size;
    else
    {
	file->search.pos = file->pos;
	if (entry != NULL)
	    parseTagLine (file, entry);
    }
    return result;
}

static tagResult findNext (tagFile *const file, tagEntry *const entry)
{
    tagResult result = TagFailure;
    if (file->sorted && ! file->search.ignorecase)
    {
	result = tagsNext (file, entry);
	if (result == TagSuccess  && nameComparison (file) != 0)
	    result = TagFailure;
    }
    else
    {
	result = findSequential (file);
	if (result == TagSuccess  &&  entry != NULL)
	    parseTagLine (file, entry);
    }
    return result;
}

/*
*  EXTERNAL INTERFACE
*/

extern tagFile *tagsOpen (const char *filePath, tagFileInfo *info)
{
    return initialize (filePath, info);
}

extern tagResult tagsSetSorted (tagFile *file, int sorted)
{
    tagResult result = TagFailure;
    if (file != NULL  &&  file->initialized)
    {
	file->sorted = sorted;
	result = TagSuccess;
    }
    return result;
}

extern tagResult tagsNext (tagFile *const file, tagEntry *const entry)
{
    tagResult result = TagFailure;
    if (file != NULL  &&  file->initialized)
	result = readNext (file, entry);
    return result;
}

extern const char *tagsField (const tagEntry *const entry, const char *const key)
{
    const char *result = NULL;
    if (entry != NULL)
	result = readFieldValue (entry, key);
    return result;
}

extern tagResult tagsFind (tagFile *const file, tagEntry *const entry,
			   const char *const name, const int options)
{
    tagResult result = TagFailure;
    if (file != NULL  &&  file->initialized)
	result = find (file, entry, name, options);
    return result;
}

extern tagResult tagsFindNext (tagFile *const file, tagEntry *const entry)
{
    tagResult result = TagFailure;
    if (file != NULL  &&  file->initialized)
	result = findNext (file, entry);
    return result;
}

extern tagResult tagsClose (tagFile *file)
{
    tagResult result = TagFailure;
    if (file != NULL  &&  file->initialized)
    {
	terminate (file);
	result = TagSuccess;
    }
    return result;
}

/*
*  TEST FRAMEWORK
*/

#ifdef READTAGS_MAIN

static const char *TagFileName = "tags";
static int extensionFields;
static int SortOverride;
static int Sorted;

static void printTag (const tagEntry *entry)
{
    int i;
    boolean first = TRUE;
    const char* separator = ";\"";
    const char* const empty = "";
/* "sep" returns a value only the first time it is evaluated */
#define sep (first ? (first = FALSE, separator) : empty)
    printf ("%s\t%s\t%s",
	entry->name, entry->file, entry->address.pattern);
    if (extensionFields)
    {
	if (entry->kind != NULL  &&  entry->kind [0] != '\0')
	    printf ("%s\tkind:%s", sep, entry->kind);
	if (entry->fileScope)
	    printf ("%s\tfile:", sep);
#if 0
	if (entry->address.lineNumber > 0)
	    printf ("%s\tline:%lu", sep, entry->address.lineNumber);
#endif
	for (i = 0  ;  i < entry->fields.count  ;  ++i)
	    printf ("%s\t%s:%s", sep, entry->fields.list [i].key,
		entry->fields.list [i].value);
    }
    putchar ('\n');
#undef sep
}

static void findTag (const char *const name, const int options)
{
    tagFileInfo info;
    tagEntry entry;
    tagFile *const file = tagsOpen (TagFileName, &info);
    if (file == NULL)
	perror ("Cannot open tag file");
    else
    {
	if (SortOverride)
	    tagsSetSorted (file, Sorted);
	if (tagsFind (file, &entry, name, options) == TagSuccess)
	{
	    do
	    {
		printTag (&entry);
	    } while (tagsFindNext (file, &entry) == TagSuccess);
	}
	tagsClose (file);
    }
}

static void listTags (void)
{
    tagFileInfo info;
    tagEntry entry;
    tagFile *const file = tagsOpen (TagFileName, &info);
    if (file == NULL)
	perror ("Cannot open tag file");
    else
    {
	while (tagsNext (file, &entry) == TagSuccess)
	    printTag (&entry);
	tagsClose (file);
    }
}

const char *const Usage =
    "Find tag file entries matching specified names.\n\n"
    "Usage: %s [-ilp] [-s[0|1]] [-t file] [name(s)]\n\n"
    "Options:\n"
    "    -e           Include extension fields in output.\n"
    "    -i           Perform case-insensitive matching.\n"
    "    -l           List all tags.\n"
    "    -p           Perform partial matching.\n"
    "    -s[0|1]      Override sort detection of tag file.\n"
    "    -t file      Use specified tag file (default: \"tags\").\n"
    "Note that options are acted upon as encountered, so order is significant.\n";

extern int main (int argc, char **argv)
{
    int i;
    int options = 0;
    if (argc == 1)
    {
	fprintf (stderr, Usage, argv [0]);
	exit (1);
    }
    for (i = 1  ;  i < argc  ;  ++i)
    {
	const char *const arg = argv [i];
	if (arg [0] == '-')
	{
	    int j;
	    for (j = 1  ;  arg [j] != '\0'  ;  ++j)
	    {
		switch (arg[j])
		{
		    case 'e': extensionFields = 1;         break;
		    case 'i': options |= TAG_IGNORECASE;   break;
		    case 'p': options |= TAG_PARTIALMATCH; break;
		    case 'l': listTags ();                 break;
		    case 't': TagFileName = argv [++i];    break;
		    case 's':
			SortOverride = 1;
			if (arg [j] == '\0')
			    Sorted = 1;
			else
			    Sorted = (arg [++j] != '0');
			break;
		    default:
			fprintf (stderr, "%s: unknown option: %c\n",
				    argv[0], arg[j]);
			exit (1);
			break;
		}
	    }
	}
	else
	    findTag (arg, options);
    }
    return 0;
}

#endif

/* vi:set tabstop=8 shiftwidth=4: */
