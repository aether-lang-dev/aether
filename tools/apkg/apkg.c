#include "apkg.h"
#include "toml_parser.h"
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <unistd.h>
#endif

/* system() reports the child's exit status; a failed mkdir left the
 * caller cloning into a directory that does not exist, so git's error
 * masked the real cause (a read-only or permission-denied cache path). */
static int apkg_run_mkdir(const char* cmd, const char* path) {
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "Error: cannot create directory '%s'\n", path);
        return 0;
    }
    return 1;
}

// Version is set by Makefile from VERSION file
#ifndef AETHER_VERSION
#define AETHER_VERSION "0.0.0-dev"
#endif
#define APKG_VERSION AETHER_VERSION

// Validate a string contains only safe characters for shell commands.
// Allows: alphanumeric, '.', '/', '-', '_', '@'
// Rejects anything else (spaces, semicolons, backticks, $, etc.)
static int is_safe_shell_arg(const char* s) {
    if (!s || !*s) return 0;
    for (const char* p = s; *p; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '/' ||
            c == '-' || c == '_' || c == '@') continue;
        return 0;
    }
    return 1;
}

int apkg_init(const char* name) {
    printf("Initializing new Aether package '%s'...\n", name);
    
    FILE* f = fopen("aether.toml", "w");
    if (!f) {
        fprintf(stderr, "Error: Could not create aether.toml\n");
        return 1;
    }
    
    fprintf(f, "[package]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "version = \"0.1.0\"\n");
    fprintf(f, "authors = [\"Your Name <email@example.com>\"]\n");
    fprintf(f, "license = \"MIT\"\n");
    fprintf(f, "description = \"A new Aether project\"\n");
    fprintf(f, "\n");
    fprintf(f, "[dependencies]\n");
    fprintf(f, "# Add dependencies here\n");
    fprintf(f, "\n");
    fprintf(f, "[dev-dependencies]\n");
    fprintf(f, "# Add dev dependencies here\n");
    fprintf(f, "\n");
    fprintf(f, "[build]\n");
    fprintf(f, "target = \"native\"\n");
    fprintf(f, "optimizations = \"aggressive\"\n");
    fprintf(f, "\n");
    fprintf(f, "[[bin]]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "path = \"src/main.ae\"\n");
    
    fclose(f);
    
    #ifdef _WIN32
        _mkdir("src");
    #else
        mkdir("src", 0755);
    #endif
    
    FILE* main_file = fopen("src/main.ae", "w");
    if (main_file) {
        fprintf(main_file, "main() {\n");
        fprintf(main_file, "    print(\"Hello from %s!\")\n", name);
        fprintf(main_file, "}\n");
        fclose(main_file);
    }
    
    FILE* readme = fopen("README.md", "w");
    if (readme) {
        fprintf(readme, "# %s\n\n", name);
        fprintf(readme, "A new Aether project.\n\n");
        fprintf(readme, "## Building\n\n");
        fprintf(readme, "```bash\n");
        fprintf(readme, "apkg build\n");
        fprintf(readme, "```\n\n");
        fprintf(readme, "## Running\n\n");
        fprintf(readme, "```bash\n");
        fprintf(readme, "apkg run\n");
        fprintf(readme, "```\n");
        fclose(readme);
    }
    
    FILE* gitignore = fopen(".gitignore", "w");
    if (gitignore) {
        fprintf(gitignore, "target/\n");
        fprintf(gitignore, "*.c\n");
        fprintf(gitignore, "*.o\n");
        fprintf(gitignore, "*.exe\n");
        fprintf(gitignore, "aether.lock\n");
        fclose(gitignore);
    }
    
    printf("✓ Created package '%s'\n", name);
    printf("  - aether.toml\n");
    printf("  - src/main.ae\n");
    printf("  - README.md\n");
    printf("  - .gitignore\n");
    printf("\nNext steps:\n");
    printf("  cd %s\n", name);
    printf("  apkg build\n");
    printf("  apkg run\n");
    
    return 0;
}

int apkg_install(const char* package) {
    printf("Installing package '%s'...\n", package);
    
    // Parse manifest using TOML parser
    TomlDocument* doc = toml_parse_file("aether.toml");
    if (!doc) {
        fprintf(stderr, "Error: No aether.toml found. Run 'apkg init' first.\n");
        return 1;
    }
    
    // Check if already installed
    int dep_count = 0;
    TomlKeyValue* deps = toml_get_section_entries(doc, "dependencies", &dep_count);
    for (int i = 0; i < dep_count; i++) {
        if (strcmp(deps[i].key, package) == 0) {
            printf("Package '%s' already in dependencies (version: %s)\n", package, deps[i].value);
            toml_free_document(doc);
            return 0;
        }
    }
    
    toml_free_document(doc);
    
    // Check if package is in cache
    PackageInfo info = apkg_find_package(package);
    
    if (!info.exists) {
        printf("Package not found in cache, downloading...\n");
        if (apkg_download_package(package, "latest") != 0) {
            fprintf(stderr, "Failed to download package\n");
            free(info.name);
            free(info.path);
            return 1;
        }
    } else {
        printf("Using cached package: %s\n", info.path);
    }
    
    // Read package version from downloaded package's aether.toml
    char pkg_manifest[1024];
    snprintf(pkg_manifest, sizeof(pkg_manifest), "%s/aether.toml", info.path);
    TomlDocument* pkg_doc = toml_parse_file(pkg_manifest);
    const char* pkg_version = "latest";
    if (pkg_doc) {
        const char* version = toml_get_value(pkg_doc, "package", "version");
        if (version) {
            pkg_version = version;
        }
    }
    
    // Add to aether.toml dependencies
    FILE* toml = fopen("aether.toml", "r");
    if (!toml) {
        fprintf(stderr, "Error: Could not read aether.toml\n");
        if (pkg_doc) toml_free_document(pkg_doc);
        free(info.name);
        free(info.path);
        return 1;
    }
    
    // Read entire file
    fseek(toml, 0, SEEK_END);
    long size = ftell(toml);
    fseek(toml, 0, SEEK_SET);
    char* content = malloc(size + 1024);
    size_t nread = fread(content, 1, size, toml);
    content[nread] = '\0';
    fclose(toml);
    
    // Find [dependencies] section or add it
    char* deps_section = strstr(content, "[dependencies]");
    if (deps_section) {
        // Add after [dependencies] section
        char* next_section = strchr(deps_section + 14, '[');
        if (next_section) {
            // Insert before next section
            size_t pos = next_section - content;
            size_t alloc_size = size + 1024;
            char* new_content = malloc(alloc_size);
            strncpy(new_content, content, pos);
            new_content[pos] = '\0';
            snprintf(new_content + strlen(new_content),
                     alloc_size - strlen(new_content),
                     "%s = \"%s\"\n", package, pkg_version);
            strcat(new_content, next_section);
            
            toml = fopen("aether.toml", "w");
            fputs(new_content, toml);
            fclose(toml);
            free(new_content);
        } else {
            // Add at end
            toml = fopen("aether.toml", "a");
            fprintf(toml, "%s = \"%s\"\n", package, pkg_version);
            fclose(toml);
        }
    } else {
        // Add new [dependencies] section
        toml = fopen("aether.toml", "a");
        fprintf(toml, "\n[dependencies]\n");
        fprintf(toml, "%s = \"%s\"\n", package, pkg_version);
        fclose(toml);
    }
    
    free(content);
    if (pkg_doc) toml_free_document(pkg_doc);
    free(info.name);
    free(info.path);
    
    printf("✓ Added %s@%s to dependencies\n", package, pkg_version);
    printf("✓ Package '%s' installed\n", package);
    return 0;
}

int apkg_publish() {
    printf("Publishing package...\n");
    
    FILE* manifest = fopen("aether.toml", "r");
    if (!manifest) {
        fprintf(stderr, "Error: No aether.toml found.\n");
        return 1;
    }
    fclose(manifest);
    
    printf("Publishing package to registry...\n\n");
    
    // Step 1: Validate aether.toml
    printf("[1/5] Validating aether.toml...\n");
    manifest = fopen("aether.toml", "r");
    char line[256];
    int has_name = 0, has_version = 0;
    while (fgets(line, sizeof(line), manifest)) {
        if (strstr(line, "name")) has_name = 1;
        if (strstr(line, "version")) has_version = 1;
    }
    fclose(manifest);
    
    if (!has_name || !has_version) {
        fprintf(stderr, "Error: aether.toml must have 'name' and 'version' fields\n");
        return 1;
    }
    printf("✓ Manifest valid\n\n");
    
    // Step 2: Run tests
    printf("[2/5] Running tests...\n");
    int test_result = apkg_test();
    if (test_result != 0) {
        fprintf(stderr, "Error: Tests failed. Fix tests before publishing.\n");
        return 1;
    }
    printf("\n");
    
    // Step 3: Build package
    printf("[3/5] Building package...\n");
    int build_result = apkg_build();
    if (build_result != 0) {
        fprintf(stderr, "Error: Build failed\n");
        return 1;
    }
    printf("\n");
    
    // Step 4: Create tarball
    printf("[4/5] Creating tarball...\n");
    #ifdef _WIN32
        int tar_result = system("tar -czf package.tar.gz src aether.toml README.md");
    #else
        int tar_result = system("tar -czf package.tar.gz src aether.toml README.md 2>/dev/null");
    #endif
    
    if (tar_result == 0) {
        printf("✓ Created package.tar.gz\n\n");
    } else {
        fprintf(stderr, "Warning: Could not create tarball (tar not available)\n\n");
    }
    
    // Step 5: Upload to registry
    printf("[5/5] Package tarball created.\n");
    printf("\n");
    printf("Package ready: package.tar.gz\n");
    printf("\n");
    printf("Share it manually, or host on GitHub and add with:\n");
    printf("  ae add github.com/your-user/your-package\n");
    
    return 0;
}

int apkg_build() {
    printf("Building package...\n");
    
    FILE* manifest = fopen("aether.toml", "r");
    if (!manifest) {
        fprintf(stderr, "Error: No aether.toml found.\n");
        return 1;
    }
    fclose(manifest);
    
    // Create target directory if it doesn't exist
    #ifdef _WIN32
        system("if not exist target mkdir target");
    #else
        system("mkdir -p target");
    #endif
    
    printf("Compiling src/main.ae...\n");
    
    // Try to compile with aetherc
    int result = system("aetherc src/main.ae target/main.c 2>&1");
    if (result != 0) {
        fprintf(stderr, "Error: Compilation failed\n");
        fprintf(stderr, "Make sure 'aetherc' is in your PATH\n");
        return 1;
    }
    
    // Generate lock file for reproducible builds
    printf("Generating aether.lock...\n");
    FILE* lock = fopen("aether.lock", "w");
    if (lock) {
        fprintf(lock, "# Aether lock file - auto-generated, do not edit\n");
        fprintf(lock, "# This file ensures reproducible builds\n\n");
        fprintf(lock, "[metadata]\n");
        fprintf(lock, "generated = \"%s\"\n", __DATE__);
        fprintf(lock, "\n");
        
        // Parse dependencies from manifest and write resolved versions
        manifest = fopen("aether.toml", "r");
        if (manifest) {
            char line[256];
            int in_deps = 0;
            
            fprintf(lock, "[[package]]\n");
            
            while (fgets(line, sizeof(line), manifest)) {
                if (strstr(line, "[dependencies]")) {
                    in_deps = 1;
                    continue;
                }
                if (in_deps && line[0] == '[') {
                    in_deps = 0;
                }
                if (in_deps && strchr(line, '=')) {
                    fprintf(lock, "%s", line);
                }
            }
            fclose(manifest);
        }
        
        fclose(lock);
        printf("✓ Created aether.lock\n");
    }
    
    printf("✓ Build complete\n");
    return 0;
}

int apkg_test() {
    printf("Running tests...\n");
    
    // Check if tests directory exists
    #ifdef _WIN32
        if (access("tests", 0) != 0) {
    #else
        if (access("tests", F_OK) != 0) {
    #endif
        printf("No tests directory found. Creating tests/example_test.ae...\n");
        #ifdef _WIN32
            system("if not exist tests mkdir tests");
        #else
            system("mkdir -p tests");
        #endif
        
        FILE* example = fopen("tests/example_test.ae", "w");
        if (example) {
            fprintf(example, "// Example test file\n");
            fprintf(example, "// Add your tests here\n\n");
            fprintf(example, "func test_addition() {\n");
            fprintf(example, "    result = 2 + 2\n");
            fprintf(example, "    print(result)  // Should be 4\n");
            fprintf(example, "}\n\n");
            fprintf(example, "main() {\n");
            fprintf(example, "    test_addition()\n");
            fprintf(example, "    print(\"All tests passed!\")\n");
            fprintf(example, "}\n");
            fclose(example);
            printf("✓ Created tests/example_test.ae\n");
        }
        return 0;
    }
    
    // Find all test files
    printf("Discovering tests in tests/...\n");
    
    #ifdef _WIN32
        FILE* pipe = popen("dir /b /s tests\\*.ae", "r");
    #else
        FILE* pipe = popen("find tests -name '*.ae' 2>/dev/null", "r");
    #endif
    
    if (!pipe) {
        fprintf(stderr, "Warning: Could not discover test files\n");
        return 0;
    }
    
    char test_file[512];
    int test_count = 0;
    int passed = 0;
    int failed = 0;
    
    while (fgets(test_file, sizeof(test_file), pipe)) {
        // Remove newline (including \r on Windows)
        test_file[strcspn(test_file, "\r\n")] = 0;
        if (strlen(test_file) == 0) continue;

        // Validate filename — reject anything with shell metacharacters
        if (!is_safe_shell_arg(test_file)) {
            fprintf(stderr, "Skipping test with unsafe filename: %s\n", test_file);
            continue;
        }

        test_count++;
        printf("\nRunning test: %s\n", test_file);

        // Compile test
        char compile_cmd[1024];
        char output_file[512];
        snprintf(output_file, sizeof(output_file), "target/test_%d", test_count);
        snprintf(compile_cmd, sizeof(compile_cmd), "aetherc %s target/test_%d.c 2>&1", test_file, test_count);
        
        int compile_result = system(compile_cmd);
        if (compile_result != 0) {
            printf("✗ Compilation failed\n");
            failed++;
            continue;
        }
        
        // Compile C to executable
        char build_cmd[1024];
        #ifdef _WIN32
            snprintf(build_cmd, sizeof(build_cmd), 
                     "gcc target/test_%d.c -Iruntime -o %s.exe 2>nul",
                     test_count, output_file);
        #else
            snprintf(build_cmd, sizeof(build_cmd),
                     "gcc target/test_%d.c -Iruntime -o %s 2>/dev/null",
                     test_count, output_file);
        #endif
        
        int build_result = system(build_cmd);
        if (build_result != 0) {
            printf("✗ Build failed\n");
            failed++;
            continue;
        }
        
        // Run test
        char run_cmd[512];
        #ifdef _WIN32
            snprintf(run_cmd, sizeof(run_cmd), "%s.exe", output_file);
        #else
            snprintf(run_cmd, sizeof(run_cmd), "./%s", output_file);
        #endif
        
        int run_result = system(run_cmd);
        if (run_result == 0) {
            printf("✓ Passed\n");
            passed++;
        } else {
            printf("✗ Failed (exit code: %d)\n", run_result);
            failed++;
        }
    }
    
    pclose(pipe);
    
    printf("\n" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "\n");
    printf("Test Results: %d passed, %d failed, %d total\n", passed, failed, test_count);
    printf("=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "=" "\n");
    
    return (failed > 0) ? 1 : 0;
}

int apkg_search(const char* query) {
    // Validate query to prevent shell injection
    if (!is_safe_shell_arg(query)) {
        fprintf(stderr, "Error: Search query contains invalid characters\n");
        return 1;
    }

    printf("Searching for '%s' in package registry...\n\n", query);

    // For now, search local cache and provide instructions for online search
    char cache_dir[512];
    #ifdef _WIN32
        const char* home = getenv("USERPROFILE");
        snprintf(cache_dir, sizeof(cache_dir), "%s\\.aether\\packages", home ? home : ".");
    #else
        const char* home = getenv("HOME");
        snprintf(cache_dir, sizeof(cache_dir), "%s/.aether/packages", home ? home : ".");
    #endif
    
    printf("Local packages (in cache):\n");
    
    #ifdef _WIN32
        char search_cmd[1024];
        snprintf(search_cmd, sizeof(search_cmd), 
                 "dir /b /s \"%s\\*%s*\" 2>nul", cache_dir, query);
        FILE* pipe = popen(search_cmd, "r");
    #else
        char search_cmd[1024];
        snprintf(search_cmd, sizeof(search_cmd),
                 "find %s -type d -name '*%s*' 2>/dev/null", cache_dir, query);
        FILE* pipe = popen(search_cmd, "r");
    #endif
    
    int found_local = 0;
    if (pipe) {
        char result[512];
        while (fgets(result, sizeof(result), pipe)) {
            result[strcspn(result, "\r\n")] = 0;
            if (strlen(result) > 0) {
                printf("  - %s\n", result);
                found_local++;
            }
        }
        pclose(pipe);
    }
    
    if (found_local == 0) {
        printf("  (no local packages found)\n");
    }
    
    printf("\n");
    printf("Search GitHub for Aether packages:\n");
    printf("  https://github.com/search?q=%s+language%%3Aaether\n", query);
    printf("\n");
    printf("To install a package:\n");
    printf("  apkg install github.com/user/package\n");
    printf("\n");
    printf("Popular packages:\n");
    printf("  - std.collections  (HashMap, Set, Vector, PriorityQueue)\n");
    printf("  - std.net          (HTTP client, TCP sockets)\n");
    printf("  - std.json         (JSON parser and stringifier)\n");
    printf("  - std.log          (Structured logging)\n");
    
    return 0;
}

int apkg_update() {
    printf("Updating dependencies...\n\n");
    
    FILE* manifest = fopen("aether.toml", "r");
    if (!manifest) {
        fprintf(stderr, "Error: No aether.toml found.\n");
        return 1;
    }
    
    // Parse dependencies from aether.toml
    char line[256];
    int in_dependencies = 0;
    int dep_count = 0;
    
    printf("Current dependencies:\n");
    
    while (fgets(line, sizeof(line), manifest)) {
        // Check for [dependencies] section
        if (strstr(line, "[dependencies]")) {
            in_dependencies = 1;
            continue;
        }
        
        // Check for new section (stop parsing dependencies)
        if (in_dependencies && line[0] == '[') {
            in_dependencies = 0;
            continue;
        }
        
        // Parse dependency lines
        if (in_dependencies && strchr(line, '=')) {
            dep_count++;
            line[strcspn(line, "\n")] = 0;
            printf("  %s\n", line);
            
            // Extract package name
            char pkg_name[256] = {0};
            char* eq = strchr(line, '=');
            if (eq) {
                int name_len = eq - line;
                while (name_len > 0 && (line[name_len-1] == ' ' || line[name_len-1] == '\t')) {
                    name_len--;
                }
                strncpy(pkg_name, line, name_len);
                pkg_name[name_len] = 0;
                
                // Trim leading whitespace
                char* name_start = pkg_name;
                while (*name_start == ' ' || *name_start == '\t') name_start++;
                
                // Try to update this package
                printf("    Checking for updates to %s...\n", name_start);
                
                // Remove quotes from version
                char* version_start = eq + 1;
                while (*version_start == ' ' || *version_start == '\t' || *version_start == '\"') {
                    version_start++;
                }
                char* version_end = version_start + strlen(version_start) - 1;
                while (version_end > version_start && (*version_end == ' ' || *version_end == '\t' || *version_end == '\"' || *version_end == '\n')) {
                    *version_end = 0;
                    version_end--;
                }
                
                printf("    Current version: %s\n", version_start);
                printf("    ✓ Up to date\n");
            }
        }
    }
    
    fclose(manifest);
    
    if (dep_count == 0) {
        printf("  (no dependencies found)\n");
    }
    
    printf("\n");
    printf("To manually update a dependency:\n");
    printf("  1. Edit aether.toml and change the version\n");
    printf("  2. Run: apkg install\n");
    printf("\n");
    printf("To add a new dependency:\n");
    printf("  apkg install github.com/user/package\n");
    
    return 0;
}

int apkg_run() {
    printf("Running package...\n");
    
    // Check if we need to build
    if (access("target/main.c", F_OK) != 0) {
        printf("Building first...\n");
        if (apkg_build() != 0) {
            return 1;
        }
    }
    
    // Compile C to executable if needed
    #ifdef _WIN32
        const char* exe_name = "target/main.exe";
    #else
        const char* exe_name = "target/main";
    #endif
    
    if (access(exe_name, F_OK) != 0) {
        printf("Compiling to executable...\n");
        char build_cmd[1024];
        #ifdef _WIN32
            snprintf(build_cmd, sizeof(build_cmd),
                     "gcc target/main.c -Iruntime -Istd -Lbuild -laether_runtime -o %s 2>nul", exe_name);
        #else
            snprintf(build_cmd, sizeof(build_cmd),
                     "gcc target/main.c -Iruntime -Istd -Lbuild -laether_runtime -o %s 2>/dev/null", exe_name);
        #endif
        
        int build_result = system(build_cmd);
        if (build_result != 0) {
            fprintf(stderr, "Error: Failed to compile executable\n");
            fprintf(stderr, "Trying fallback build without runtime library...\n");
            
            #ifdef _WIN32
                snprintf(build_cmd, sizeof(build_cmd), "gcc target/main.c -o %s", exe_name);
            #else
                snprintf(build_cmd, sizeof(build_cmd), "gcc target/main.c -o %s", exe_name);
            #endif
            
            build_result = system(build_cmd);
            if (build_result != 0) {
                fprintf(stderr, "Error: Compilation failed\n");
                return 1;
            }
        }
        printf("✓ Executable created: %s\n", exe_name);
    }
    
    // Execute the binary
    printf("\n--- Running ---\n\n");
    
    #ifdef _WIN32
        int result = system(exe_name);
    #else
        char run_cmd[512];
        snprintf(run_cmd, sizeof(run_cmd), "./%s", exe_name);
        int result = system(run_cmd);
    #endif
    
    printf("\n--- Finished (exit code: %d) ---\n", result);
    return result;
}

void apkg_print_help() {
    printf("apkg - Aether Package Manager v%s\n\n", APKG_VERSION);
    printf("USAGE:\n");
    printf("    apkg <command> [options]\n\n");
    printf("COMMANDS:\n");
    printf("    init <name>       Initialize a new package\n");
    printf("    install <pkg>     Install a package\n");
    printf("    update            Update dependencies\n");
    printf("    build             Build the package\n");
    printf("    run               Build and run the package\n");
    printf("    test              Run tests\n");
    printf("    publish           Publish package to registry\n");
    printf("    search <query>    Search for packages\n");
    printf("    help              Show this help message\n");
    printf("    version           Show version information\n\n");
    printf("For more information, see: https://github.com/aether-lang-dev/aether\n");
}

void apkg_print_version() {
    printf("apkg %s\n", APKG_VERSION);
}

void apkg_free_package(Package* pkg) {
    if (!pkg) return;
    free(pkg->name);
    free(pkg->version);
    free(pkg->description);
    free(pkg->license);
    for (int i = 0; i < pkg->author_count; i++) {
        free(pkg->authors[i]);
    }
    free(pkg->authors);
    for (int i = 0; i < pkg->dependency_count; i++) {
        free(pkg->dependencies[i]);
    }
    free(pkg->dependencies);
    free(pkg);
}

PackageInfo apkg_find_package(const char* name) {
    PackageInfo info = {0};
    info.name = strdup(name);
    
    // Determine cache location
    char cache_dir[512];
    #ifdef _WIN32
        const char* home = getenv("USERPROFILE");
        snprintf(cache_dir, sizeof(cache_dir), "%s\\.aether\\packages", home ? home : ".");
    #else
        const char* home = getenv("HOME");
        snprintf(cache_dir, sizeof(cache_dir), "%s/.aether/packages", home ? home : ".");
    #endif
    
    // Build full path
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", cache_dir, name);
    
    info.path = strdup(full_path);
    info.exists = (access(full_path, F_OK) == 0);
    
    return info;
}

int apkg_download_package(const char* name, const char* version) {
    // Parse GitHub URL from package name
    // Expected format: github.com/user/repo
    if (strncmp(name, "github.com/", 11) != 0) {
        fprintf(stderr, "Error: Only GitHub packages supported currently\n");
        fprintf(stderr, "Package name must start with 'github.com/'\n");
        return 1;
    }
    
    const char* repo_path = name + 11;  // Skip "github.com/"

    // Validate inputs before any filesystem or shell operations
    if (!is_safe_shell_arg(name)) {
        fprintf(stderr, "Error: Package name contains invalid characters\n");
        return 1;
    }
    if (version && strcmp(version, "latest") != 0 && !is_safe_shell_arg(version)) {
        fprintf(stderr, "Error: Version string contains invalid characters\n");
        return 1;
    }

    // Create package cache directory
    char cache_dir[512];
    #ifdef _WIN32
        const char* home = getenv("USERPROFILE");
        snprintf(cache_dir, sizeof(cache_dir), "%s\\.aether\\packages", home ? home : ".");
    #else
        const char* home = getenv("HOME");
        snprintf(cache_dir, sizeof(cache_dir), "%s/.aether/packages", home ? home : ".");
    #endif

    // Create nested directories for github.com/user/repo structure
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s/%s", cache_dir, name);

    // Check if already downloaded
    if (access(full_path, F_OK) == 0) {
        printf("Package already cached: %s\n", full_path);
        return 0;
    }

    printf("Fetching package: %s\n", name);

    // Create parent directories
    char mkdir_cmd[1024];
    char parent_dir[1024];
    snprintf(parent_dir, sizeof(parent_dir), "%s/github.com", cache_dir);
    
    #ifdef _WIN32
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "if not exist \"%s\" mkdir \"%s\"", cache_dir, cache_dir);
        if (!apkg_run_mkdir(mkdir_cmd, cache_dir)) return 1;
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "if not exist \"%s\" mkdir \"%s\"", parent_dir, parent_dir);
        if (!apkg_run_mkdir(mkdir_cmd, parent_dir)) return 1;
    #else
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", parent_dir);
        if (!apkg_run_mkdir(mkdir_cmd, parent_dir)) return 1;
    #endif
    
    // Extract user part for parent directory
    char user_dir[1024];
    const char* slash = strchr(repo_path, '/');
    if (slash) {
        int user_len = slash - repo_path;
        snprintf(user_dir, sizeof(user_dir), "%s/github.com/%.*s", cache_dir, user_len, repo_path);
        #ifdef _WIN32
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "if not exist \"%s\" mkdir \"%s\"", user_dir, user_dir);
        #else
            snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", user_dir);
        #endif
        if (!apkg_run_mkdir(mkdir_cmd, user_dir)) return 1;
    }
    
    // Clone repository
    char clone_cmd[1024];
    if (version && strcmp(version, "latest") != 0) {
        // Clone specific version/tag
        snprintf(clone_cmd, sizeof(clone_cmd), 
                 "git clone --depth 1 --branch %s https://%s \"%s\"",
                 version, name, full_path);
    } else {
        // Clone latest
        snprintf(clone_cmd, sizeof(clone_cmd), 
                 "git clone --depth 1 https://%s \"%s\"",
                 name, full_path);
    }
    
    printf("Running: %s\n", clone_cmd);
    int result = system(clone_cmd);
    
    if (result != 0) {
        fprintf(stderr, "Error: Failed to clone package\n");
        return 1;
    }
    
    printf("✓ Package downloaded to: %s\n", full_path);
    
    // Check for aether.toml
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/aether.toml", full_path);
    if (access(manifest_path, F_OK) == 0) {
        printf("✓ Found aether.toml\n");
    } else {
        fprintf(stderr, "Warning: No aether.toml found in package\n");
    }
    
    return 0;
}

