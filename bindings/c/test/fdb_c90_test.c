#define FDB_API_VERSION 740
#include <foundationdb/fdb_c.h>

int main(int argc, char* argv[]) {
	(void)argc;
	(void)argv;
	fdb_select_api_version(FDB_API_VERSION);
	return 0;
}
