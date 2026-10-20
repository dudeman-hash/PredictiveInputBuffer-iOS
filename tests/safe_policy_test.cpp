#include "../src/safe_policy.hpp"
#include <cassert>
#include <iostream>
int main() {
    using namespace pib;
    assert(!blockKnownSave(true, false, false, false, true));
    assert(blockKnownSave(true, true, false, false, true));
    assert(!blockKnownSave(false, true, false, false, true));
    assert(!blockKnownSave(true, true, true, false, true));
    assert(!blockKnownSave(true, true, false, true, true));
    assert(!blockKnownSave(true, true, false, false, false));
    bool setting = false;
    assert(!blockKnownSave(setting, true, false, false, true));
    setting = true;
    assert(blockKnownSave(setting, true, false, false, true));
    assert(!allowIntervention(true, 0, false, false, false));
    assert(!allowIntervention(false, 8, false, false, false));
    assert(!allowIntervention(true, 8, true, false, false));
    assert(allowIntervention(true, 8, true, true, false));
    assert(allowIntervention(true, 8, false, false, false));
    std::cout << "Safe policy: 13 checks passed\n";
}
