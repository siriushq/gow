#define _GNU_SOURCE
#define _BSD_SOURCE
#define _SYSV_SOURCE
#define _COSMO_SOURCE

#include <cosmo.h>

#include <sys/wait.h>
#include <sys/utsname.h>

#include <libc/proc/ntspawn.h>
#include <libc/runtime/runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <libgen.h>
#include <errno.h>

static char* system_name;
static char* system_architecture;
static char* system_format;
static bool system_windows;

#define PLEDGES \
	"stdio rpath wpath cpath exec proc prot_exec inet dns"

#define POWERSHELL \
	"/C/Windows/System32/WindowsPowerShell/v1.0/powershell.exe"

#define ERROR_BOOTLOADER \
	("gow cannot be run in bootloader mode\n")
#define ERROR_UNSUPPORTED \
	("gow requires Windows or UNIX-compatible system\n" \
	"(attempting to assume UNIX compatibility)\n")
#define ERROR_UNAME \
	("gow failed to invoke `uname` on your UNIX-compatible system\n")

#define COMMAND_DEPENDENCIES (system_windows ? \
	POWERSHELL " -NoProfile -c \"exit\"" : \
	"curl -V > /dev/null && tar --help > /dev/null")
#define ERROR_DEPENDENCIES (system_windows ? \
	"gow requires `powershell` to be available\n" : \
	"gow requires `curl` and `tar` to be available in PATH\n")

#define COMMAND_VERSION (system_windows ? \
	POWERSHELL " -c " \
		"\"iwr 'https://go.dev/VERSION?m=text' -ea stop" \
		" | Select-Object -Expand Content\"" : \
	"curl -s https://go.dev/VERSION?m=text")

#define COMMAND_DOWNLOAD (system_windows ? \
	POWERSHELL " -c \"iwr '%s' -o '%s' -ea stop\"" : \
	"curl -f -sS %s -o %s")
#define ERROR_DOWNLOAD (system_windows ? \
	"gow failed at PowerShell `iwr` (POSIX `system` returned -1)\n" : \
	"gow failed at `curl` (POSIX `system` returned -1)\n")
#define FAILURE_DOWNLOAD (system_windows ? \
	"gow received a failure code from PowerShell (`iwr`, %d)\n" : \
	"gow received a failure code from `curl` (%d)\n")

#define COMMAND_EXTRACT (system_windows ? \
	POWERSHELL " -c " \
		"\"Add-Type -A System.IO.Compression.FileSystem;" \
		"[IO.Compression.ZipFile]::ExtractToDirectory('%s','%s');\"" : \
	"tar -xzf %s -C %s")
#define ERROR_EXTRACT (system_windows ? \
	"gow failed at PowerShell decompress (POSIX `system` returned -1)\n" : \
	"gow failed at `tar` (POSIX `system` returned -1)\n")
#define FAILURE_EXTRACT (system_windows ? \
	"gow received a failure code from PowerShell (decompress, %d)\n" : \
	"gow received a failure code from `tar` (%d)\n")

/// Populates #system_name, #system_format and #system_architecture
/// with the values from the currently running system.
///
/// Returns `false` on error.
static bool detect_system(void)
{
	if (IsMetal()) {
		fprintf(stderr, ERROR_BOOTLOADER);
		return false;
	}
	if (IsWindows()) {
		system_architecture = _ARCH_NAME;
		system_format = "zip";
		system_name = "windows";
		system_windows = true;
		return true;
	}

	if (!SupportsSystemv()) fprintf(stderr, ERROR_UNSUPPORTED);

	struct utsname name;
	if (uname(&name) == -1) {
		fprintf(stderr, ERROR_UNAME);
		return false;
	}

	system_windows = false;
	system_format = "tar.gz";
	system_architecture =
		!strcmp(name.machine, "x86_64") ? "amd64" :
		!strcmp(name.machine, "i386") ? "386" :
		!strcmp(name.machine, "i686") ? "386" :
		!strcmp(name.machine, "aarch64") ? "arm64" :
		!strcmp(name.machine, "armv7l") ? "arm" :
		!strcmp(name.machine, "armv6l") ? "arm" :
		!strcmp(name.machine, "ppc64le") ? "ppc64le" :
		!strcmp(name.machine, "ppc64") ? "ppc64" :
		!strcmp(name.machine, "riscv64") ? "riscv64" :
		!strcmp(name.machine, "s390x") ? "s390x" :
		"unknown";
	system_name =
		!strcmp(name.sysname, "Linux") ? "linux" :
		!strcmp(name.sysname, "Darwin") ? "darwin" :
		!strcmp(name.sysname, "FreeBSD") ? "freebsd" :
		!strcmp(name.sysname, "OpenBSD") ? "openbsd" :
		!strcmp(name.sysname, "NetBSD") ? "netbsd" :
		!strcmp(name.sysname, "DragonFly") ? "dragonfly" :
		!strcmp(name.sysname, "SunOS") ? "solaris" :
		!strcmp(name.sysname, "AIX") ? "aix" :
		"unknown";
	return true;
}

/// Detects whether prerequisites are met pre-execution, and supplies
/// platform-dependent error messages if missing.
///
/// Returns `false` on error.
static bool detect_dependencies(void)
{
	int error = system(COMMAND_DEPENDENCIES);
	if (error == -1) {
		fprintf(stderr, ERROR_DEPENDENCIES);
		return false;
	}
	return true;
}

/// Queries `go.mod` for a `//gow <version>\n` directive, assumed to
/// be declared (precisely) at the very top of the file.
///
/// @see #detect_version
/// @return `NULL` or version
static char* detect_version_module(void)
{
	char executable[PATH_MAX];
	strncpy(executable, __program_executable_name, PATH_MAX);
	executable[sizeof(executable) - 1] = 0;

	char module[PATH_MAX];
	snprintf(module, PATH_MAX, "%s/go.mod", dirname(executable));

	char line[256];
	FILE* file = fopen(module, "r");
	if (!file) return NULL;
	
	if (!fgets(line, sizeof(line), file)) goto failure;
	
	char* directive = "//gow ";
	if (strncmp(line, directive, strlen(directive))) goto failure;

	line[strcspn(line, "\n")] = 0;
	fclose(file);
	return strdup(line + strlen(directive));

	failure: {
		fclose(file);
		return NULL;
	}
}

/// Return the desired version of go, in the following order:
///
/// - Attempting to query for a custom parameter set in the `go.mod` of
///   the directory which contains the current executable (`gow.com`)
/// - Assuming a "known good" version (of 1.25.5), returning it for any
///   further errors
/// - Attempting to query (and parse response of) the download server for
///   the latest available version of Go
///
/// The returned string must be `free`d.
static char* detect_version(void)
{	
	char* module = detect_version_module();
	if (module != NULL) return module;

	char* known = "1.25.5";
	FILE* file = popen(COMMAND_VERSION, "r");
	if (!file) return strdup(known);

	char buffer[256];
	if (!fgets(buffer, sizeof(buffer), file)) {
		pclose(file);
		return strdup(known);
	}
	pclose(file);

	char* prefix = strstr(buffer, "go");
	if (!prefix) return strdup(known);
	prefix += strlen("go");

	char* end = prefix;
	while (*end &&
			*end != ' ' &&
			*end != '\n' &&
			*end != '\r') end++;
	*end = 0;
	return strdup(prefix);
}

/// Execute a command using POSIX `system(...)`.
///
/// @param error An error message if the shell invocation fails.
/// @param failure A failure message if the command itself fails.
///
/// @param command A format string containing the command to run,
/// then `...` variadic argument using `vasprintf` for substitution
/// (ignoring return value, as failed allocation will exit safely).
///
/// @return `true` if no error occurred
static bool execute(char* error, char* failure, char* command, ...)
{
	va_list arguments;
	va_start(arguments, command);

	char* formatted;
	vasprintf(&formatted, command, arguments);
	
	va_end(arguments);

	int result = system(formatted);
	free(formatted);
	
	if (result == -1) {
		fprintf(stderr, error);
		return false;
	} else if (WIFEXITED(result) && WEXITSTATUS(result) != 0) {
		result = WEXITSTATUS(result);
		fprintf(stderr, failure, result);
		return false;
	}
	return true;
}

/// Calls `pledge` from Cosmopolitan `libc`, which delegates to pledge
/// on OpenBSD, seccomp on Linux, and does nothing on other platforms.
static bool lockdown()
{
	if (pledge(PLEDGES, PLEDGES) == -1) {
		perror("pledge");
		return false;
	}
	return true;
}

int main(int argc, char** argv)
{
	if (!lockdown()) goto failure;
	if (!detect_system()) goto failure;
	if (!detect_dependencies()) goto failure;

	char* path = NULL;
	char* remote = "https://dl.google.com/go/go";
	char* version = detect_version();

	asprintf(&path, "%s%s.%s-%s.%s",
			remote,
			version,
			system_name,
			system_architecture,
			system_format);

	char* home = getenv("HOME");
	if (!home) goto failure;

	char directory[PATH_MAX];
	snprintf(directory, PATH_MAX, "%s/.gow/%s/", home, version);

	char directory_mungent[PATH_MAX];
	strncpy(directory_mungent, directory, PATH_MAX);
	if (system_windows) mungentpath(directory_mungent);

	if (access(directory, F_OK) != 0) {
		printf("\x1b[s");
		printf("installing Go %s (in ~/.gow/)...\n", version);
		fflush(stdout);

		printf("MAKEDIRS, for '%s' >>>\n", directory);
		int error = makedirs(directory, 0755);
		if (error != 0 && errno != EEXIST) goto failure;
		printf("MAKEDIRS <<<\n");

		char download[PATH_MAX];
		snprintf(download, PATH_MAX, "%sdownload.%s",
				directory, system_format);
		if (system_windows) mungentpath(download);
		
		printf("DOWNLOAD >>>\n");
		if (!execute(ERROR_DOWNLOAD, FAILURE_DOWNLOAD, COMMAND_DOWNLOAD,
				path, download)) goto failure;
		printf("DOWNLOAD <<<\n");
		printf("EXTRACT >>>\n");
		if (!execute(ERROR_EXTRACT, FAILURE_EXTRACT, COMMAND_EXTRACT,
				download, directory_mungent)) goto failure;
		printf("EXTRACT <<<\n");
		if (remove(download) != 0) goto failure;

		printf("\x1b[u");
		printf("\rinstalled Go %-20s\n", version);
		fflush(stdout);
	}

	char go[PATH_MAX];
	snprintf(go, PATH_MAX, "%sgo/bin/%s",
			directory, (system_windows ? "go.exe" : "go"));

	char goroot[PATH_MAX];
	snprintf(goroot, PATH_MAX, "%sgo/", directory);
	if (system_windows) mungentpath(goroot);
	setenv("GOROOT", goroot, true);

	char gopath[PATH_MAX];
	snprintf(gopath, PATH_MAX, "%srepository/", directory);
	if (system_windows) mungentpath(gopath);
	setenv("GOPATH", gopath, true);

	execv(go, argv);
	perror("execv");
	goto failure;

	failure: {
		free(version);
		free(path);
		return EXIT_FAILURE;
	}
}