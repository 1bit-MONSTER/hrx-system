# Copyright 2026 The IREE Authors
#
# Licensed under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

"""EXPECT domain — runtime check.expect mismatches."""

from loom.errors import ErrorDef, ErrorDomain, Severity

# ERR_EXPECT_001: Exact equality expectation mismatch.
ERR_EXPECT_001 = ErrorDef(
    domain=ErrorDomain.EXPECT,
    code=1,
    severity=Severity.ERROR,
    summary="Equality expectation did not match.",
    message="check.expect.equal did not match",
    params=(),
)

# ERR_EXPECT_002: Bitwise equality expectation mismatch.
ERR_EXPECT_002 = ErrorDef(
    domain=ErrorDomain.EXPECT,
    code=2,
    severity=Severity.ERROR,
    summary="Bitwise expectation did not match.",
    message="check.expect.bitwise did not match",
    params=(),
)

# ERR_EXPECT_003: Approximate comparison expectation mismatch.
ERR_EXPECT_003 = ErrorDef(
    domain=ErrorDomain.EXPECT,
    code=3,
    severity=Severity.ERROR,
    summary="Close expectation did not match.",
    message="check.expect.close did not match",
    params=(),
)

# ERR_EXPECT_004: Shape expectation mismatch.
ERR_EXPECT_004 = ErrorDef(
    domain=ErrorDomain.EXPECT,
    code=4,
    severity=Severity.ERROR,
    summary="Shape expectation did not match.",
    message="check.expect.shape did not match",
    params=(),
)

# ERR_EXPECT_005: Device event expectation mismatch.
ERR_EXPECT_005 = ErrorDef(
    domain=ErrorDomain.EXPECT,
    code=5,
    severity=Severity.ERROR,
    summary="Device event expectation did not match.",
    message="check.expect.event did not match",
    params=(),
)

ALL_EXPECT_ERRORS: tuple[ErrorDef, ...] = (
    ERR_EXPECT_001,
    ERR_EXPECT_002,
    ERR_EXPECT_003,
    ERR_EXPECT_004,
    ERR_EXPECT_005,
)
