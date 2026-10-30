"""Run with generated Python on PYTHONPATH. Old-reader wire compatibility."""
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory
from kiarabot.v1 import plugins_pb2 as plugins, common_pb2 as common

pool = descriptor_pool.DescriptorPool()


def add(descriptor):
    for dependency in descriptor.dependencies:
        try:
            pool.FindFileByName(dependency.name)
        except KeyError:
            add(dependency)
    copy = descriptor_pb2.FileDescriptorProto()
    descriptor.CopyToProto(copy)
    if descriptor.name.endswith('/plugins.proto'):
        removed = {'RegisterPluginRequest': {3}, 'RegisterPluginResponse': {5},
                   'PluginToCore': {6, 7}, 'CoreToPlugin': {7}, 'PluginActionRequest': {2}}
        for message in copy.message_type:
            retained = [field for field in message.field if field.number not in removed.get(message.name, set())]
            del message.field[:]
            message.field.extend(retained)
    pool.Add(copy)


add(plugins.DESCRIPTOR)
legacy_request = message_factory.GetMessageClass(pool.FindMessageTypeByName('kiarabot.v1.RegisterPluginRequest'))
legacy_response = message_factory.GetMessageClass(pool.FindMessageTypeByName('kiarabot.v1.RegisterPluginResponse'))
request = plugins.RegisterPluginRequest(instance_id='instance', supported_features=['action.result.v1'])
old = legacy_request.FromString(request.SerializeToString())
assert old.instance_id == 'instance'
# Unknown fields survive a legacy parse/serialize hop.
assert plugins.RegisterPluginRequest.FromString(old.SerializeToString()).supported_features == ['action.result.v1']
old_response = legacy_response(accepted=True, required_api_version='kiarabot.plugin.v1')
assert not plugins.RegisterPluginResponse.FromString(old_response.SerializeToString()).enabled_features
for selector in ('action_id', 'idempotency_key'):
    query = plugins.PluginToCore(action_query=plugins.PluginActionQuery(request_id='query', **{selector: 'key'}))
    decoded = plugins.PluginToCore.FromString(query.SerializeToString())
    assert decoded.action_query.WhichOneof('selector') == selector
response = plugins.PluginActionResponse(request_id='query')
response.result.status = 12345
assert plugins.PluginActionResponse.FromString(response.SerializeToString()).result.status == 12345
response.error.code = common.TIMEOUT
assert response.WhichOneof('outcome') == 'error' and not response.HasField('result')
print('Action contract legacy-reader, negotiation defaults, selectors and unknown outcome passed')
