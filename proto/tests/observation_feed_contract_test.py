"""v1.2 observation-feed contract oracles only; not producer/consumer evidence.

Validates the frozen logs/tasks/messages feed shapes: shared request presence,
response boundaries, closed safe projections, terminal task visibility and the
absence of leakage fields. Data-producing/consuming behavior is verified by the
owner and Control tests, never by this file.
"""
from kiarabot.v1 import common_pb2, observations_pb2

LOG_SEVERITIES = {'DEBUG', 'INFO', 'WARN', 'ERROR'}
LOG_EVENTS = {'request.recorded', 'request.rejected'}
LOG_OWNER_SERVICES = {'identity', 'runtime'}
TASK_OWNER_SERVICES = {'scheduler'}
MESSAGE_OWNER_SERVICES = {'runtime'}

LOG_METADATA_FIELDS = {
    'owner_service', 'entry_id', 'severity', 'component', 'method', 'event',
    'resource', 'request_id', 'trace_id', 'occurred_at'}
TASK_METADATA_FIELDS = {
    'owner_service', 'task_id', 'owner_instance_id', 'state', 'version',
    'updated_at', 'cancel_requested', 'occurrence_id', 'reason_code'}
MESSAGE_METADATA_FIELDS = {
    'owner_service', 'message_id', 'conversation_id', 'account_id', 'received_at'}

# Fields that must never appear in the safe change projections.
FORBIDDEN_SUBSTRINGS = (
    'payload', 'body', 'content', 'platform', 'raw', 'secret', 'token',
    'message_text', 'target', 'segments', 'sent')


def fields_of(message):
    return set(message.DESCRIPTOR.fields_by_name)


def declared_order(message):
    return [field.name for field in message.DESCRIPTOR.fields]


def assert_no_forbidden(message_name, allowed):
    for name in allowed:
        for banned in FORBIDDEN_SUBSTRINGS:
            assert banned not in name, (message_name, name, banned)


def check_observation_feed_contract():
    request = observations_pb2.ObservationChangesRequest
    assert declared_order(request) == ['context', 'scope', 'cursor', 'page_size']
    assert request.DESCRIPTOR.fields_by_name['cursor'].has_presence
    assert request.DESCRIPTOR.fields_by_name['context'].message_type.full_name == \
        'kiarabot.v1.RequestContext'
    assert request.DESCRIPTOR.fields_by_name['scope'].message_type.full_name == \
        'kiarabot.v1.ResourceScope'
    # The v1.1 operation journal request stays exactly as reviewed; v1.2 must not
    # silently reinterpret it.
    operation_request = observations_pb2.OperationChangesRequest
    assert declared_order(operation_request) == ['context', 'scope', 'cursor', 'page_size']
    assert declared_order(observations_pb2.OperationChange) == ['operation', 'cursor']
    assert declared_order(observations_pb2.OperationChangesResponse) == \
        ['changes', 'next_cursor', 'has_more', 'checked_at']

    assert fields_of(observations_pb2.LogMetadata) == LOG_METADATA_FIELDS
    assert fields_of(observations_pb2.TaskMetadata) == TASK_METADATA_FIELDS
    assert fields_of(observations_pb2.MessageMetadata) == MESSAGE_METADATA_FIELDS
    assert_no_forbidden('LogMetadata', LOG_METADATA_FIELDS - {'resource', 'method'})
    assert_no_forbidden('TaskMetadata', TASK_METADATA_FIELDS)
    assert_no_forbidden('MessageMetadata', MESSAGE_METADATA_FIELDS)

    # Task state must use the shared closed ScheduledTaskState enum and keep all
    # terminal values expressible; FAILED carries a stable ErrorCode, not text.
    task_state = observations_pb2.TaskMetadata.DESCRIPTOR.fields_by_name['state']
    assert task_state.enum_type.full_name == 'kiarabot.v1.ScheduledTaskState'
    valid_states = {value.name for value in task_state.enum_type.values}
    for terminal in ('SCHEDULED_TASK_COMPLETED', 'SCHEDULED_TASK_FAILED',
                     'SCHEDULED_TASK_CANCELLED'):
        assert terminal in valid_states, terminal
    assert valid_states == {value.name for value in common_pb2.ScheduledTaskState.DESCRIPTOR.values}
    reason = observations_pb2.TaskMetadata.DESCRIPTOR.fields_by_name['reason_code']
    assert reason.has_presence
    assert reason.enum_type.full_name == 'kiarabot.v1.ErrorCode'

    # Every change carries exactly its metadata plus a per-change cursor, and the
    # response boundary fields are mandatory (next_cursor presence via message type).
    for change, metadata_name, metadata_type in (
            (observations_pb2.LogChange, 'log', 'LogMetadata'),
            (observations_pb2.TaskChange, 'task', 'TaskMetadata'),
            (observations_pb2.MessageMetadataChange, 'message', 'MessageMetadata')):
        assert declared_order(change) == [metadata_name, 'cursor'], declared_order(change)
        assert change.DESCRIPTOR.fields_by_name[metadata_name].message_type.full_name == \
            'kiarabot.v1.' + metadata_type
        cursor = change.DESCRIPTOR.fields_by_name['cursor']
        assert cursor.message_type.full_name == 'kiarabot.v1.StreamCursor'

    for response_name in ('LogChangesResponse', 'TaskChangesResponse',
                          'MessageMetadataChangesResponse'):
        response = getattr(observations_pb2, response_name)
        assert declared_order(response) == ['changes', 'next_cursor', 'has_more', 'checked_at']
        assert response.DESCRIPTOR.fields_by_name['next_cursor'].message_type.full_name == \
            'kiarabot.v1.StreamCursor'

    # Closed vocabularies that producers must enforce and consumers must reject
    # free text against (documented in the proto; string type keeps v1-compatible
    # log projection without inventing a new enum surface for v1.2).
    assert LOG_SEVERITIES == {'DEBUG', 'INFO', 'WARN', 'ERROR'}
    assert LOG_EVENTS == {'request.recorded', 'request.rejected'}
    assert LOG_OWNER_SERVICES == {'identity', 'runtime'}
    assert TASK_OWNER_SERVICES == {'scheduler'}
    assert MESSAGE_OWNER_SERVICES == {'runtime'}

    # No sent direction or platform identity can be represented by the message
    # metadata projection: only internal ids and the receive time.
    assert 'direction' not in MESSAGE_METADATA_FIELDS
    assert 'received_at' in MESSAGE_METADATA_FIELDS
    print('v1.2 observation feed request/response/safe-projection contract checks passed')


if __name__ == '__main__':
    check_observation_feed_contract()
