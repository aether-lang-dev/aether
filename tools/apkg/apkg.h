#ifndef APKG_H
#define APKG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "toml_parser.h"

typedef struct {
    char* name;
    char* version;
    char* description;
    char* license;
    char** authors;
    int author_count;
    char** dependencies;
    int dependency_count;
} Package;

typedef struct {
    char* name;
    char* path;
    int exists;
} PackageInfo;

int apkg_init(const char* name);
int apkg_install(const char* package);
int apkg_publish(void);
int apkg_build(void);
int apkg_test(void);
int apkg_search(const char* query);
int apkg_update(void);
int apkg_run(void);

void apkg_free_package(Package* pkg);

PackageInfo apkg_find_package(const char* name);
int apkg_download_package(const char* name, const char* version);

void apkg_print_help(void);
void apkg_print_version(void);

#endif

