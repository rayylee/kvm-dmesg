/* version.c
 *
 * Copyright (C) 2024 Ray Lee
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "version.h"
#include "defs.h"
#include "log.h"
#include <stdlib.h>

#define vquote(x) #x
#define vexp(x) vquote(x)

#define VERSION_TEXT (vexp(MAJOR_VERSION) "." vexp(MINOR_VERSION) "." vexp(BUGFIX_VERSION) EXTRA_VERSION)
#define VERSION_ID ((MAJOR_VERSION) * 10000 + (MINOR_VERSION) * 100 + (BUGFIX_VERSION))

const char* get_version_text()
{
    return VERSION_TEXT;
}

void
parse_kernel_version(char *str)
{
	char *p1, *p2, separator;

	p1 = p2 = str;
    printf("%s %d, str: %s\n", __func__, __LINE__, str);
	while (*p2 != '.' && *p2 != '\0')
		p2++;

	*p2 = NULLCHAR;
	kt->kernel_version[0] = atoi(p1);
	p1 = ++p2;
	while (*p2 != '.' && *p2 != '-' && *p2 != '\0')
		p2++;

	separator = *p2;
	*p2 = NULLCHAR;
	kt->kernel_version[1] = atoi(p1);

	if (separator == '.') {
		p1 = ++p2;
		while ((*p2 >= '0') && (*p2 <= '9'))
			p2++;

		*p2 = NULLCHAR;
		kt->kernel_version[2] = atoi(p1);
	}

	fprintf(fp, "Linux version: v%d.%d.%d\n", kt->kernel_version[0],
			kt->kernel_version[1], kt->kernel_version[2]);
}
