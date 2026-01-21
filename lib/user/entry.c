#include <syscall.h>

int main (int, char *[]);
void _start (int argc, char *argv[]);
extern const char *test_name __attribute__((weak));

void
_start (int argc, char *argv[]) {
	if (&test_name != NULL && test_name == NULL && argc > 0)
		test_name = argv[0];
	exit (main (argc, argv));
}
