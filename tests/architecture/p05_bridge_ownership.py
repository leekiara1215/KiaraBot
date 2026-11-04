"""Cross-instance and replacement registration checks inside the real PG/Runtime Bridge fixture."""
import asyncio
import hashlib
import time
import uuid


async def check_parallel_ownership(sql, adapter_factory, plugin_factory, adapter_template,
                                   plugin_template, event_template, primary_adapter, primary_account,
                                   primary_installation, secondary_plugin, plugin_secret,
                                   primary_streams, read_frame, expect_error, require):
    import grpc
    from kiarabot.v1 import platform_pb2, plugins_pb2

    # Add a separately bound account and a second installation to the already running fixture.
    adapter_instance, adapter_id, account, installation = (str(uuid.uuid4()) for _ in range(4))
    secret = uuid.uuid4().hex + uuid.uuid4().hex
    digest = hashlib.sha256(secret.encode()).hexdigest()
    sql(f"""
        BEGIN;
        INSERT INTO kiarabot.accounts(id,platform,platform_account_id,display_name,version,state)
          SELECT '{account}',platform,'20002','Second adapter',1,state
          FROM kiarabot.accounts WHERE id='{primary_account}';
        INSERT INTO kiarabot.adapter_bindings(account,adapter_instance,version,state)
          SELECT '{account}','{adapter_instance}',1,state FROM kiarabot.adapter_bindings
          WHERE account='{primary_account}' AND adapter_instance='{primary_adapter}';
        INSERT INTO kiarabot.bridge_credentials(id,resource_id,digest,created_at,revoked,version)
          VALUES ('{uuid.uuid4()}','{adapter_instance}',decode('{digest}','hex'),
                  (extract(epoch from clock_timestamp())*1000000000)::bigint,false,1);
        INSERT INTO kiarabot.installations(id,plugin_type,version,state,manifest)
          SELECT '{installation}',plugin_type,1,state,manifest FROM kiarabot.installations
          WHERE id='{primary_installation}';
        UPDATE kiarabot.instances SET installation='{installation}' WHERE id='{secondary_plugin}';
        COMMIT;
    """)
    adapter = type(adapter_template)()
    adapter.CopyFrom(adapter_template)
    adapter.register.adapter_id = adapter_id
    adapter.register.adapter_instance_id = adapter_instance
    adapter.register.accounts[0].account_id = account
    plugin = type(plugin_template)()
    plugin.CopyFrom(plugin_template)
    plugin.register.instance_id = secondary_plugin
    plugin.register.installation_id = installation

    def renew_context(registration):
        if type(registration) is type(adapter):
            context = registration.register.context
            context.request_id = str(uuid.uuid4())
            context.trace_id = str(uuid.uuid4())
            context.deadline.GetCurrentTime()
            context.deadline.seconds += 5

    async def connect(factory, credential, registration):
        renew_context(registration)
        call = factory(metadata=(('authorization', 'Bearer ' + credential),))
        await call.write(registration)
        registered = await read_frame(call, 'additional bridge registration')
        require(registered.WhichOneof('body') == 'register_result' and
                registered.register_result.accepted,
                f'additional {type(registration).__name__} registration rejected '
                f'(code={registered.register_result.error.code})')
        return call, registered.register_result.authorization

    async def refresh(call, frame_type, auth):
        await call.write(frame_type(
            fence=auth.fence, connection_proof=auth.proof,
            refresh_authorization=platform_pb2.RefreshConnectionAuthorization(
                request_id=str(uuid.uuid4()), fence=auth.fence, proof=auth.proof,
                refresh_credential=auth.refresh_credential,
                refresh_generation=auth.refresh_generation)))

    wrong_installation = type(plugin)()
    wrong_installation.CopyFrom(plugin)
    wrong_installation.register.installation_id = primary_installation
    for factory, credential, claimed in (
            (adapter_factory, secret, adapter_template),
            (plugin_factory, plugin_secret, plugin_template),
            (plugin_factory, plugin_secret, wrong_installation)):
        wrong = factory(metadata=(('authorization', 'Bearer ' + credential),))
        renew_context(claimed)
        await wrong.write(claimed)
        await expect_error(wrong, 'cross-instance registration', {grpc.StatusCode.PERMISSION_DENIED})

    peers = [(adapter_factory, secret, adapter), (plugin_factory, plugin_secret, plugin)]
    old = await asyncio.gather(*(connect(*peer) for peer in peers))
    # Each primary still owns its reply writer, even after the secondary streams opened.
    for (_, _, frame_template), (primary, auth) in zip(peers, primary_streams):
        heartbeat = type(frame_template)(fence=auth.fence, connection_proof=auth.proof)
        heartbeat.heartbeat.instance_id = auth.fence.instance_id
        heartbeat.heartbeat.sent_at_unix_ms = int(time.time() * 1000)
        await primary.write(heartbeat)
        ack = await read_frame(primary, 'original connection heartbeat', timeout=5)
        require(ack.WhichOneof('body') == 'heartbeat_ack' and ack.fence == auth.fence,
                'heartbeat reply went to the wrong stream')

    new = await asyncio.gather(*(connect(*peer) for peer in peers))
    for peer, (old_call, old_auth), (new_call, new_auth), expected in zip(
            peers, old, new, (adapter_instance, secondary_plugin)):
        require(new_auth.fence.instance_id == old_auth.fence.instance_id == expected and
                new_auth.fence.connection_generation == old_auth.fence.connection_generation + 1,
                'replacing one bridge instance must only advance its own generation')
        if expected == adapter_instance:
            event = type(event_template)()
            event.CopyFrom(event_template)
            event.event_id = str(uuid.uuid4())
            event.source.adapter_id = adapter_id
            event.source.adapter_instance_id = adapter_instance
            event.source.account_id = account
            await old_call.write(type(adapter)(fence=old_auth.fence, connection_proof=old_auth.proof,
                                               event=event))
        else:
            await old_call.write(type(plugin)(fence=old_auth.fence, connection_proof=old_auth.proof,
                                              heartbeat=plugins_pb2.PluginHeartbeat(instance_id=expected)))
        await expect_error(old_call, 'superseded bridge frame', {grpc.StatusCode.UNAUTHENTICATED})
        if expected == adapter_instance:
            rows = sql(f"SELECT count(*) FROM kiarabot.events WHERE event_id='{event.event_id}'")
            require(rows.stdout.strip() == b'0', 'superseded adapter event reached the inbox')
        await refresh(new_call, type(peer[2]), new_auth)
        frame = await read_frame(new_call, 'current bridge authorization')
        require(frame.authorization_result.authorization.refresh_generation ==
                new_auth.refresh_generation + 1, 'current bridge lost its authorization')
        renewed = frame.authorization_result.authorization

        async def racing_refresh():
            try:
                await refresh(new_call, type(peer[2]), renewed)
                result = await read_frame(new_call, 'racing refresh')
                require(result.authorization_result.HasField('authorization'),
                        'an accepted refresh must return a credential')
            except grpc.RpcError as error:
                require(error.code() == grpc.StatusCode.UNAUTHENTICATED,
                        'a replaced stream must lose its refresh authority')

        (winner, latest), _ = await asyncio.gather(connect(*peer), racing_refresh())
        require(latest.fence.connection_generation == new_auth.fence.connection_generation + 1,
                'replacement must advance the generation even during refresh')
        await refresh(winner, type(peer[2]), latest)
        result = await read_frame(winner, 'replacement after racing refresh')
        require(result.authorization_result.authorization.fence == latest.fence,
                'the superseded refresh must not replace the current connection')
        new_call.cancel()
        winner.cancel()
