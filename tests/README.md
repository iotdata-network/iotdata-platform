# tests/

    unit/   the host suites: no radio, no second machine, no bench.

UNIT TESTS LIVE WITH THE CODE, which is the ordinary rule and why these are here. The ones that
need a LAB -- several boards, a radio between them, a filesystem that survives a power cycle --
live in `iotdata-testing/platform/`, because they belong to no single repository and would make
this one depend upward on the examples and devices that instantiate it.

The two are not alternatives. A unit suite can fake a store but cannot switch a node off; the lab
can switch it off but cannot reach inside it. Most behaviours here want both, and where a rule is
checked twice on purpose the comment says so.
