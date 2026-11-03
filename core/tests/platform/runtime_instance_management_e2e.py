"""Exact resource management for an existing deployment; creation is independently gated."""
import uuid


def grant_existing_objects(enrollment, installation, instance):
    for kind, ident, permissions in [
        ('RESOURCE_INSTALLATION', installation, ['plugin.read']),
        ('RESOURCE_INSTANCE', instance, ['plugin.read', 'plugin.manage', 'plugin.config.read']),
    ]:
        for grants in ['administrator_assignments', 'control_assignments']:
            enrollment[grants].append({'assignment_id': str(uuid.uuid4()),
                                       'permissions': permissions,
                                       'scope': [{'kind': kind, 'id': ident}]})


def verify(stub, issue, runtime_sql, installation_id, instance_id, context, require, run_sql):
    import grpc
    from kiarabot.v1 import platform_pb2 as p, plugins_pb2, policy_pb2 as perm, runtime_pb2 as r

    install = p.ResourceRef(kind=p.RESOURCE_INSTALLATION, id=installation_id)
    inst = p.ResourceRef(kind=p.RESOURCE_INSTANCE, id=instance_id)
    plugin_type = str(uuid.uuid4())
    manifest = plugins_pb2.PluginManifest(id=plugin_type, name='managed-test', version='1.0.0',
                                           api_version='kiarabot.v1', runtime='python-rpc',
                                           entrypoint='plugin:create')
    # Only pre-existing resource data is provisioned. All auth, reads, mutations and retries
    # below cross the production Control-certificate -> Runtime -> Identity mTLS boundary.
    run_sql(runtime_sql, f"INSERT INTO kiarabot.installations(id,plugin_type,version,state,manifest) "
                         f"VALUES('{installation_id}','{plugin_type}',1,0,"
                         f"decode('{manifest.SerializeToString().hex()}','hex')); "
                         f"INSERT INTO kiarabot.instances(id,installation,plugin_type,desired_version,"
                         f"applied_config_version,version,state,desired) VALUES('{instance_id}',"
                         f"'{installation_id}','{plugin_type}',1,0,1,0,1);")
    def ctx(ref, permission):
        return context(delegation=issue([permission], [ref]).delegation.proof)

    def rejected(call, request, code):
        try:
            call(request, timeout=5)
        except grpc.RpcError as error:
            require(error.code() == code, f'expected {code}, got {error.code()}')
        else:
            raise AssertionError('invalid management request accepted')

    get_install = r.RuntimeGetInstallationRequest(context=ctx(install, perm.PERMISSION_PLUGIN_READ),
                                                   resource=install)
    installed = stub.GetInstallation(get_install, timeout=5)
    require(installed.installation.manifest == manifest and installed.installation.version == 1 and
            installed.installation.installation_id == installation_id and
            installed.HasField('observed_at'), 'existing installation projection')
    get_instance = r.RuntimeGetInstanceRequest(context=ctx(inst, perm.PERMISSION_PLUGIN_READ),
                                                resource=inst)
    prior = stub.GetInstance(get_instance, timeout=5).instance
    require(prior.instance_id == instance_id and prior.installation_id == installation_id and
            prior.externally_managed and prior.state.desired == p.DESIRED_STOPPED and
            prior.state.version == 1 and not prior.state.HasField('fence'), 'existing instance')
    get_config = r.RuntimeGetInstanceConfigRequest(context=ctx(inst, perm.PERMISSION_PLUGIN_CONFIG_READ),
                                                    resource=inst)
    rejected(stub.GetInstanceConfig, get_config, grpc.StatusCode.NOT_FOUND)

    desired = r.RuntimeSetDesiredStateRequest(context=ctx(inst, perm.PERMISSION_PLUGIN_MANAGE),
                                              instance_id=instance_id, expected_version=1,
                                              desired_state=p.DESIRED_RUNNING)
    accepted = stub.SetDesiredState(desired, timeout=5)
    require(accepted.instance.state.desired == p.DESIRED_RUNNING and
            accepted.instance.state.version == 2 and not accepted.instance.state.HasField('fence') and
            accepted.operation.resource == inst, 'desired intent cannot synthesize an execution')
    replay = stub.SetDesiredState(desired, timeout=5)
    require(replay.operation == accepted.operation and replay.instance.state.version == 2,
            'replay retains operation and resource version')
    desired.context.CopyFrom(ctx(inst, perm.PERMISSION_PLUGIN_MANAGE))
    rejected(stub.SetDesiredState, desired, grpc.StatusCode.ABORTED)

    patch = r.RuntimeUpdateConfigRequest(context=ctx(inst, perm.PERMISSION_PLUGIN_MANAGE),
                                         resource=inst, expected_version=2)
    patch.config.update({'greeting': 'hello'})
    saved = stub.UpdateConfig(patch, timeout=5)
    require(saved.operation.resource == inst and saved.state.desired_config_version == 2,
            'config update stages new desired version')
    require(stub.UpdateConfig(patch, timeout=5) == saved, 'replay retains staged config receipt')
    patch.context.CopyFrom(ctx(inst, perm.PERMISSION_PLUGIN_MANAGE))
    rejected(stub.UpdateConfig, patch, grpc.StatusCode.ABORTED)
    cfg = stub.GetInstanceConfig(get_config, timeout=5).config
    require(cfg.values == patch.config and cfg.instance == inst and cfg.desired_version == 2 and
            cfg.applied_version == 0 and cfg.apply_state == r.CONFIG_APPLY_PENDING and
            not cfg.HasField('fence'), 'config stays pending until a fenced ACK')
    require(stub.GetInstance(get_instance, timeout=5).instance.state.version == 3,
            'same-key replay and stale writes do not advance version')
    for call, req in [(stub.GetInstallation, get_install), (stub.GetInstance, get_instance),
                      (stub.GetInstanceConfig, get_config)]:
        valid_kind = req.resource.kind
        req.resource.kind = p.RESOURCE_ACCOUNT
        rejected(call, req, grpc.StatusCode.INVALID_ARGUMENT)
        req.resource.kind = valid_kind
    foreign = str(uuid.uuid4())
    for call, req in [(stub.GetInstallation, get_install), (stub.GetInstance, get_instance),
                      (stub.GetInstanceConfig, get_config), (stub.UpdateConfig, patch)]:
        req.resource.id = foreign
        rejected(call, req, grpc.StatusCode.PERMISSION_DENIED)
    desired.instance_id = foreign
    rejected(stub.SetDesiredState, desired, grpc.StatusCode.PERMISSION_DENIED)
