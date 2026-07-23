#ifndef SCLIENT_TESTS_SUPPORT_TESTASSERTIONS_H
#define SCLIENT_TESTS_SUPPORT_TESTASSERTIONS_H

#include <cmath>
#include <iostream>
#include <string>

namespace sclient {
namespace tests {
namespace support {

/** 断言条件为真，失败时输出消息 */
inline bool Expect(bool condition, const std::string &message) {
    if (condition) {
        return true;
    }

    std::cerr << "test failure: " << message << std::endl;
    return false;
}

/** 断言浮点数在容差范围内相等 */
inline bool ExpectNear(double actual, double expected, double tolerance, const std::string &message) {
    if (std::fabs(actual - expected) <= tolerance) {
        return true;
    }

    std::cerr << "test failure: " << message
              << " actual=" << actual
              << " expected=" << expected
              << " tolerance=" << tolerance << std::endl;
    return false;
}

}  // namespace support
}  // namespace tests
}  // namespace sclient

#endif  // SCLIENT_TESTS_SUPPORT_TESTASSERTIONS_H
