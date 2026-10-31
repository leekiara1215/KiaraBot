from __future__ import annotations

import json
import uuid
from collections.abc import Mapping

from kiarabot.v1 import common_pb2 as common, events_pb2 as events


class AdapterError(Exception):
    """Only the stable code is exposed in logs or diagnostics."""

    def __init__(self, code: int):
        self.code = code
        super().__init__(common.ErrorCode.Name(code))


def identifier(value: object) -> str:
    if isinstance(value, bool) or not isinstance(value, (str, int)):
        raise AdapterError(common.INVALID_ARGUMENT)
    result = str(value)
    if not result or len(result) > 128 or not all(
        ch.isascii() and (ch.isalnum() or ch in '-_.:/@') for ch in result
    ):
        raise AdapterError(common.INVALID_ARGUMENT)
    return result


def platform_id(value: object) -> str:
    """OneBot IDs are positive integers; conversation IDs keep the native number only.

    The same value is used for inbound MessageReceived and outbound SendMessage so Core
    and plugins can echo the event conversation without rewriting it. Group and private
    conversations are distinguished by ConversationRef.type, not by an ID prefix.
    """
    result = identifier(value)
    if not result.isdigit() or len(result) > 20 or int(result) <= 0:
        raise AdapterError(common.INVALID_ARGUMENT)
    return result


def map_message(raw: Mapping, adapter_id: str, account_id: str) -> events.EventEnvelope:
    try:
        return _map_message(raw, identifier(adapter_id), identifier(account_id))
    except AdapterError:
        raise
    except (KeyError, TypeError, ValueError, OverflowError):
        raise AdapterError(common.INVALID_ARGUMENT) from None


def map_request(raw: Mapping, adapter_id: str, account_id: str) -> events.EventEnvelope:
    """Map a OneBot V11 friend request into a FriendRequest event.

    The emitted request_id is the deterministic event id (adapter-scoped safe
    token); the adapter records the platform flag under it so ApproveRequest can
    resolve the raw flag later without exposing it to plugins.
    """
    try:
        return _map_request(raw, identifier(adapter_id), identifier(account_id))
    except AdapterError:
        raise
    except (KeyError, TypeError, ValueError, OverflowError):
        raise AdapterError(common.INVALID_ARGUMENT) from None


def _map_request(raw: Mapping, adapter_id: str, account_id: str) -> events.EventEnvelope:
    if raw.get('post_type') != 'request' or raw.get('request_type') != 'friend':
        raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
    if identifier(raw['self_id']) != account_id:
        raise AdapterError(common.PERMISSION_DENIED)
    user = platform_id(raw['user_id'])
    identifier(raw['flag'])
    comment = raw.get('comment', '')
    if not isinstance(comment, str) or len(comment.encode()) > 256:
        raise AdapterError(common.INVALID_ARGUMENT)
    timestamp = raw['time']
    if isinstance(timestamp, bool) or not isinstance(timestamp, int) or not 0 <= timestamp <= 253402300799:
        raise AdapterError(common.INVALID_ARGUMENT)
    identity = json.dumps([adapter_id, account_id, str(raw['flag']), timestamp],
                          separators=(',', ':'))
    request_id = str(uuid.uuid5(uuid.NAMESPACE_URL, 'kiarabot:onebot:v11:request:' + identity))
    result = events.EventEnvelope(event_id=request_id, trace_id=request_id,
                                  schema_version='kiarabot.v1',
                                  delivery_mode=common.AT_LEAST_ONCE)
    result.occurred_at.seconds = timestamp
    result.source.adapter_id = adapter_id
    result.source.account_id = account_id
    result.subject.user_id = user
    payload = result.friend_request
    payload.user_id = user
    payload.comment = comment
    payload.request_id = request_id
    return result


def _map_message(raw: Mapping, adapter_id: str, account_id: str) -> events.EventEnvelope:
    if raw.get('post_type') != 'message' or raw.get('message_type') not in ('group', 'private'):
        raise AdapterError(common.CAPABILITY_NOT_SUPPORTED)
    if identifier(raw['self_id']) != account_id:
        raise AdapterError(common.PERMISSION_DENIED)
    user = platform_id(raw['user_id'])
    group = platform_id(raw['group_id']) if raw['message_type'] == 'group' else ''
    conversation = group if group else user
    message_id = identifier(raw['message_id'])
    timestamp = raw['time']
    if isinstance(timestamp, bool) or not isinstance(timestamp, int) or not 0 <= timestamp <= 253402300799:
        raise AdapterError(common.INVALID_ARGUMENT)
    identity = json.dumps([adapter_id, account_id, conversation, message_id, timestamp], separators=(',', ':'))
    event_id = str(uuid.uuid5(uuid.NAMESPACE_URL, 'kiarabot:onebot:v11:' + identity))
    result = events.EventEnvelope(event_id=event_id, trace_id=event_id, schema_version='kiarabot.v1',
                                  delivery_mode=common.AT_LEAST_ONCE)
    result.occurred_at.seconds = timestamp
    result.source.adapter_id = adapter_id
    result.source.account_id = account_id
    result.subject.user_id = user
    result.subject.conversation_id = conversation
    result.subject.group_id = group
    payload = result.message_received
    payload.message_id = message_id
    payload.sender_id = user
    payload.conversation.id = conversation
    payload.conversation.account_id = account_id
    payload.conversation.type = common.ConversationRef.GROUP if group else common.ConversationRef.PRIVATE
    segments = raw.get('original_message', raw['message'])
    if not isinstance(segments, list) or len(segments) > 1024:
        raise AdapterError(common.INVALID_ARGUMENT)
    for segment in segments:
        if not isinstance(segment, dict) or not isinstance(segment.get('data'), dict):
            raise AdapterError(common.INVALID_ARGUMENT)
        kind, data = segment.get('type'), segment['data']
        output = payload.message.segments.add()
        if kind == 'text':
            if not isinstance(data.get('text'), str):
                raise AdapterError(common.INVALID_ARGUMENT)
            output.type = common.MessageSegment.TEXT
            output.text = data['text']
            payload.message.plain_text += output.text
        elif kind == 'image':
            uri = data.get('url', data.get('file', ''))
            if not isinstance(uri, str) or len(uri) > 4096:
                raise AdapterError(common.INVALID_ARGUMENT)
            if uri.startswith(('https://', 'http://')):
                output.type, output.uri = common.MessageSegment.IMAGE, uri
            else:
                output.type = common.MessageSegment.UNKNOWN
        elif kind == 'at':
            output.type, output.id = common.MessageSegment.MENTION, identifier(data['qq'])
            payload.mentions.append(output.id)
        elif kind == 'reply':
            output.type, output.id = common.MessageSegment.REPLY, identifier(data['id'])
            payload.reply_to_message_id = output.id
        else:
            output.type = common.MessageSegment.UNKNOWN
    if result.ByteSize() > 1_000_000:
        raise AdapterError(common.INVALID_ARGUMENT)
    return result
