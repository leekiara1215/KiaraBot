"""P11c SDK model.chat contract tests (no process/network: Context with a fake RPC)."""
import uuid

import pytest
from kiarabot.v1 import common_pb2 as common, plugins_pb2 as plugins
from kiarabot_sdk import PluginError
from kiarabot_sdk.runtime import Context


def context(rpc):
    value = Context()
    value._rpc = rpc
    value._features = frozenset({'host.model.v1'})
    return value


async def test_generates_stable_logical_call_id():
    sent = []

    async def rpc(frame, timeout):
        sent.append(frame)
        response = plugins.CoreToPlugin(host_response=plugins.HostResponse(
            request_id=frame.host_request.request_id,
            model_chat=plugins.ModelChatResponse(text='hi', input_tokens=2, output_tokens=3)))
        return response

    result = await context(rpc).model_chat('default', [{'role': 'user', 'text': 'hello'}])
    assert result.text == 'hi'
    assert len(sent) == 1
    request = sent[0].host_request
    assert request.model_chat.model == 'default'
    assert len(request.model_chat.logical_call_id) == 36
    uuid.UUID(request.model_chat.logical_call_id)
    assert request.model_chat.logical_call_id != request.request_id
    assert request.HasField('deadline')


async def test_retry_reuses_explicit_logical_call_id():
    call_id = str(uuid.uuid4())
    seen = []

    async def rpc(frame, timeout):
        seen.append(frame.host_request.model_chat.logical_call_id)
        return plugins.CoreToPlugin(host_response=plugins.HostResponse(
            request_id=frame.host_request.request_id,
            model_chat=plugins.ModelChatResponse(text='done')))

    context(rpc)
    await context(rpc).model_chat('default', [{'role': 'user', 'text': 'hello'}],
                                  logical_call_id=call_id)
    await context(rpc).model_chat('default', [{'role': 'user', 'text': 'hello'}],
                                  logical_call_id=call_id)
    assert seen == [call_id, call_id]


@pytest.mark.parametrize('kwargs', [{'stream': True}, {'logical_call_id': 'not-a-uuid'},
                                    {'timeout': 0}])
async def test_rejects_invalid_model_arguments(kwargs):
    async def rpc(frame, timeout):
        raise AssertionError('invalid call must not reach the runtime')

    with pytest.raises(PluginError) as rejected:
        await context(rpc).model_chat('default', [{'role': 'user', 'text': 'hello'}], **kwargs)
    assert rejected.value.code in (common.CAPABILITY_NOT_SUPPORTED, common.INVALID_ARGUMENT)


async def test_unknown_outcome_stays_retryable_and_other_errors_do_not():
    async def rpc(frame, timeout):
        raise PluginError(common.UPSTREAM_UNAVAILABLE, True)

    with pytest.raises(PluginError) as retryable:
        await context(rpc).model_chat('default', [{'role': 'user', 'text': 'x'}])
    assert retryable.value.code == common.UPSTREAM_UNAVAILABLE
    assert retryable.value.retryable is True

    async def denied_rpc(frame, timeout):
        raise PluginError(common.PERMISSION_DENIED, True)

    with pytest.raises(PluginError) as denied:
        await context(denied_rpc).model_chat('default', [{'role': 'user', 'text': 'x'}])
    assert denied.value.code == common.PERMISSION_DENIED
    assert denied.value.retryable is False


async def test_host_error_is_returned_as_stable_code():
    async def rpc(frame, timeout):
        return plugins.CoreToPlugin(host_response=plugins.HostResponse(
            request_id=frame.host_request.request_id,
            error=common.Error(code=common.RATE_LIMITED, message='', retryable=True)))

    with pytest.raises(PluginError) as limited:
        await context(rpc).model_chat('default', [{'role': 'user', 'text': 'x'}])
    assert limited.value.code == common.RATE_LIMITED
    assert limited.value.retryable is True
