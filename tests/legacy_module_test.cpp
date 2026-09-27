#include "prism/launch/module.hpp"
#include <cassert>

int main(int argc, char **argv)
{
    assert(argc == 2);
    prism::launch::AppModule module(argv[1]);
    assert(module.Api().create && module.Api().destroy && !module.Api().on_instance_event);
}
