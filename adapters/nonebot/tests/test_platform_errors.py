from kiarabot.v1 import common_pb2 as common
from kiarabot_adapter_nonebot.platform_errors import classify_retcode


def test_retcode_classification():
    cases = {
        1200: common.RATE_LIMITED,
        1201: common.RATE_LIMITED,
        200: common.PERMISSION_DENIED,
        1401: common.PERMISSION_DENIED,
        1403: common.PERMISSION_DENIED,
        1404: common.NOT_FOUND,
        2000: common.NOT_FOUND,
        100: common.UPSTREAM_UNAVAILABLE,
        1500: common.UPSTREAM_UNAVAILABLE,
        None: common.UPSTREAM_UNAVAILABLE,
        '1404': common.UPSTREAM_UNAVAILABLE,
        True: common.UPSTREAM_UNAVAILABLE,
    }
    for retcode, expected in cases.items():
        assert classify_retcode(retcode) == expected, retcode
