#include <cstdlib>
#include <string>

#include "libmini.h"
#include "utils/sqlite.h"

int main(int argc, char* argv[])
{
    if (argc < 3) {
        return 2;  // 用法错误，便于排查
    }

    const std::string db_path = argv[1];
    const std::string op = argv[2];

    if (op == "acquire_then_exit") {
        libmini::SqliteDatabase db(db_path, libmini::SqliteDatabase::OpenReadWrite |
                                             libmini::SqliteDatabase::OpenCreate);
        if (!db.is_open()) {
            return 3;
        }

        libmini::SqliteWriteMutex wm(db_path);
        if (!wm.acquire(2000)) {
            return 1;  // 拿不到写锁：跨进程互斥生效
        }
        wm.release();
        return 0;  // 拿到了写锁：跨进程互斥未生效
    }

    return 2;
}
