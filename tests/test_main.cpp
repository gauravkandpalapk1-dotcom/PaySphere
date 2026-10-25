#include "TestFramework.h"

#include <exception>
#include <iostream>

int main() {
    std::size_t failed = 0;
    const auto& tests = pstest::registry();

    for (const auto& test : tests) {
        try {
            test.body();
            std::cout << "[ PASS ] " << test.name << '\n';
            continue;
        } catch (const pstest::AssertionFailure& failure) {
            std::cout << "[ FAIL ] " << test.name << "\n         " << failure.what() << '\n';
        } catch (const std::exception& error) {
            std::cout << "[ FAIL ] " << test.name << "\n         unexpected exception: "
                      << error.what() << '\n';
        } catch (...) {
            std::cout << "[ FAIL ] " << test.name << "\n         unknown exception\n";
        }
        ++failed;
    }

    std::cout << '\n' << (tests.size() - failed) << '/' << tests.size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}
