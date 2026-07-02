// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

#include "XlogApplication.h"

int main(int argc, char* argv[]) {
    auto app = XlogApplication::create();
    return app->run(argc, argv);
}
