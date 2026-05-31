#include "XlogApplication.h"

int main(int argc, char* argv[]) {
    auto app = XlogApplication::create();
    return app->run(argc, argv);
}
