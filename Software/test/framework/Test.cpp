/*************************************************************
 * Created by J. Weij
 *************************************************************/

#include <stdio.h>

#include "Test.h"

namespace
{
    struct Entry
    {
        const char*    suite;
        const char*    name;
        cctest::TestFn fn;
    };

    const int maxTests = 512;
    Entry     tests[maxTests];
    int       testCount     = 0;
    int       checkFailures = 0;
} // namespace

namespace cctest
{
    Registrar::Registrar(const char* suite, const char* name, TestFn fn)
    {
        if (testCount < maxTests)
        {
            tests[testCount].suite = suite;
            tests[testCount].name  = name;
            tests[testCount].fn    = fn;
            testCount++;
        }
    }

    void Fail(const char* file, int line, const char* expr)
    {
        checkFailures++;
        printf("    FAIL  %s:%d  %s\n", file, line, expr);
    }

    void FailEq(const char* file, int line, const char* expr, long long lhs, long long rhs)
    {
        checkFailures++;
        printf("    FAIL  %s:%d  %s  (lhs=%lld rhs=%lld)\n", file, line, expr, lhs, rhs);
    }

    int RunAll()
    {
        int passed = 0;
        int failed = 0;

        for (int i = 0; i < testCount; i++)
        {
            checkFailures = 0;
            printf("[ RUN  ] %s.%s\n", tests[i].suite, tests[i].name);
            tests[i].fn();
            if (checkFailures == 0)
            {
                printf("[  OK  ] %s.%s\n", tests[i].suite, tests[i].name);
                passed++;
            }
            else
            {
                printf("[ FAIL ] %s.%s\n", tests[i].suite, tests[i].name);
                failed++;
            }
        }

        printf("\n%d passed, %d failed, %d total\n", passed, failed, testCount);
        return failed == 0 ? 0 : 1;
    }
} // namespace cctest

int main()
{
    return cctest::RunAll();
}
