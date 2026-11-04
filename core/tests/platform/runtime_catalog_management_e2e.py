"""Approved catalog contract against real Identity delegation and Runtime PostgreSQL owners."""
import uuid

FAMILIES = ['accounts', 'installations', 'instances', 'adapters', 'hosts']
CATALOGS = {family: f'aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaa{index}'
            for index, family in enumerate(FAMILIES, 1)}


def prepare(enrollment, installation, account):
    for family, permissions in [('accounts', ['account.read', 'account.manage']),
                                ('installations', ['plugin.read', 'plugin.manage']),
                                ('instances', ['plugin.read']), ('adapters', ['adapter.read'])]:
        enrollment['administrator_assignments'].append({
            'assignment_id': str(uuid.uuid4()), 'permissions': permissions,
            'scope': [{'kind': 'RESOURCE_CATALOG', 'id': CATALOGS[family]}]})
    enrollment['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['account.read', 'account.manage'],
        'scope': [{'kind': 'RESOURCE_ACCOUNT', 'id': account}]})
    enrollment['administrator_assignments'].append({
        'assignment_id': str(uuid.uuid4()), 'permissions': ['plugin.manage'],
        'scope': [{'kind': 'RESOURCE_INSTALLATION', 'id': installation}]})


def seed_existing(database, installation, instance, run_sql):
    from kiarabot.v1 import plugins_pb2
    plugin_type = str(uuid.uuid4())
    manifest = plugins_pb2.PluginManifest(id=plugin_type, name='existing-catalog', version='1.0.0',
        api_version='kiarabot.v1', runtime='python-rpc', entrypoint='plugin:create')
    # Only Runtime objects are pre-provisioned. Identity grants were explicitly enrolled offline.
    run_sql(database, f"INSERT INTO kiarabot.installations(id,plugin_type,version,state,manifest) "
        f"VALUES('{installation}','{plugin_type}',1,0,decode('{manifest.SerializeToString().hex()}','hex')); "
        f"INSERT INTO kiarabot.instances(id,installation,plugin_type,desired_version,"
        f"applied_config_version,version,state,desired) VALUES('{instance}',"
        f"'{installation}','{plugin_type}',1,0,1,0,1);")


def verify(api, issue, account, installation, instance, context, require):
    import grpc
    from kiarabot.v1 import platform_pb2 as p, plugins_pb2, policy_pb2 as perm, runtime_pb2 as r

    def ref(kind, ident):
        return p.ResourceRef(kind=kind, id=ident)

    def catalog(family):
        return ref(p.RESOURCE_CATALOG, CATALOGS[family])

    def ctx(permission, *scope):
        return context(delegation=issue([permission], list(scope)).delegation.proof)

    def rejected(method, request, code, refresh_context=True):
        if refresh_context:
            request.context.CopyFrom(context(delegation=request.context.delegation))
        try:
            method(request, timeout=8)
        except grpc.RpcError as error:
            if error.code() != code:
                import sys
                print(f'P05 exact resource failure at catalog {method._method.decode()}: expected={code.name} actual={error.code().name}', file=sys.stderr)
            require(error.code() == code, 'catalog rejection status')
        else:
            raise AssertionError('catalog request unexpectedly allowed')

    manage_accounts = ctx(perm.PERMISSION_ACCOUNT_MANAGE, catalog('accounts'))
    create = r.RuntimeCreateAccountRequest(context=manage_accounts, catalog=catalog('accounts'),
                                          platform='qq', platform_account_id='catalog-test',
                                          display_name='catalog-created')
    created = api.CreateAccount(create, timeout=8).account
    require(bool(created.account_id), 'catalog creation returns account identity')
    # The create grant is only a directory grant. It never becomes a child read grant.
    rejected(api.GetAccount, r.RuntimeGetAccountRequest(context=manage_accounts,
             resource=ref(p.RESOURCE_ACCOUNT, created.account_id)), grpc.StatusCode.PERMISSION_DENIED)
    missing = r.RuntimeCreateAccountRequest()
    missing.CopyFrom(create)
    missing.ClearField('catalog')
    rejected(api.CreateAccount, missing, grpc.StatusCode.INVALID_ARGUMENT)
    foreign = r.RuntimeCreateAccountRequest()
    foreign.CopyFrom(create)
    foreign.catalog.CopyFrom(catalog('instances'))
    rejected(api.CreateAccount, foreign, grpc.StatusCode.PERMISSION_DENIED)

    # Both a directory scope and the explicit pre-enrolled child scope are required.
    accounts = r.RuntimeListAccountsRequest(context=ctx(perm.PERMISSION_ACCOUNT_READ,
                catalog('accounts'), ref(p.RESOURCE_ACCOUNT, account)), catalog=catalog('accounts'))
    accounts.page.page_size = 1
    listed = api.ListAccounts(accounts, timeout=20)
    require([item.account_id for item in listed.accounts] == [account], 'catalog plus exact child list')
    require(listed.snapshot.expires_at.seconds - listed.snapshot.observed_at.seconds == 600,
            'catalog fixed ten minute snapshot')
    # A lost first response retries with the same request_id and the original immutable facts.
    changed = api.UpdateAccount(r.RuntimeUpdateAccountRequest(
        context=ctx(perm.PERMISSION_ACCOUNT_MANAGE, ref(p.RESOURCE_ACCOUNT, account)),
        account_id=account, expected_version=listed.accounts[0].version,
        display_name='changed-after-snapshot'), timeout=8).account
    require(changed.display_name == 'changed-after-snapshot', 'exact account mutation between list retries')
    require(api.ListAccounts(accounts, timeout=20) == listed,
            'same first-page request_id replays original rows and snapshot metadata')
    rebound = r.RuntimeListAccountsRequest()
    rebound.CopyFrom(accounts)
    rebound.page.page_size = 2
    rejected(api.ListAccounts, rebound, grpc.StatusCode.ABORTED, refresh_context=False)
    fresh = r.RuntimeListAccountsRequest()
    fresh.CopyFrom(accounts)
    fresh.context.CopyFrom(ctx(perm.PERMISSION_ACCOUNT_READ, catalog('accounts'), ref(p.RESOURCE_ACCOUNT, account)))
    current = api.ListAccounts(fresh, timeout=20)
    require(current.snapshot.snapshot_id != listed.snapshot.snapshot_id and
            current.accounts[0].display_name == changed.display_name,
            'new first-page request_id establishes a fresh data view')
    accounts.context.CopyFrom(ctx(perm.PERMISSION_ACCOUNT_READ, catalog('accounts')))
    require(not api.ListAccounts(accounts, timeout=20).accounts, 'directory alone reveals no child')
    accounts.context.CopyFrom(ctx(perm.PERMISSION_ACCOUNT_READ, ref(p.RESOURCE_ACCOUNT, account)))
    rejected(api.ListAccounts, accounts, grpc.StatusCode.PERMISSION_DENIED)

    parent = ref(p.RESOURCE_INSTALLATION, installation)
    child = ref(p.RESOURCE_INSTANCE, instance)
    for method, message, family, child_ref, permission, collection, field in [
        (api.ListInstallations, r.RuntimeListInstallationsRequest, 'installations', parent,
         perm.PERMISSION_PLUGIN_READ, 'installations', 'installation_id'),
        (api.ListInstances, r.RuntimeListInstancesRequest, 'instances', child,
         perm.PERMISSION_PLUGIN_READ, 'instances', 'instance_id')]:
        query = message(context=ctx(permission, catalog(family), child_ref), catalog=catalog(family))
        query.page.page_size = 10
        reply = method(query, timeout=20)
        require([getattr(item, field) for item in getattr(reply, collection)] == [child_ref.id],
                'catalog typed plugin list exact visibility')
    adapters = r.RuntimeListAdaptersRequest(context=ctx(perm.PERMISSION_ADAPTER_READ,
                catalog('adapters')), catalog=catalog('adapters'))
    adapters.page.page_size = 10
    require(not api.ListAdapters(adapters, timeout=20).adapters, 'adapter directory grants no child')

    plugin_type = str(uuid.uuid4())
    manifest = plugins_pb2.PluginManifest(id=plugin_type, name='catalog-test', version='1.0.0',
        api_version='kiarabot.v1', runtime='python-rpc', entrypoint='plugin:create')
    manage_plugins = ctx(perm.PERMISSION_PLUGIN_MANAGE, catalog('installations'))
    validated = api.ValidateManifest(r.RuntimeValidateManifestRequest(context=manage_plugins,
        catalog=catalog('installations'), manifest=manifest), timeout=8)
    require(validated.valid, 'catalog manifest validation')
    manage_plugins.CopyFrom(ctx(perm.PERMISSION_PLUGIN_MANAGE, catalog('installations')))
    installed = api.CreateInstallation(r.RuntimeCreateInstallationRequest(context=manage_plugins,
        catalog=catalog('installations'), plugin_type_id=plugin_type, manifest=manifest), timeout=8)
    require(bool(installed.installation.installation_id), 'catalog installation creation')
    rejected(api.GetInstallation, r.RuntimeGetInstallationRequest(context=manage_plugins,
        resource=ref(p.RESOURCE_INSTALLATION, installed.installation.installation_id)),
        grpc.StatusCode.PERMISSION_DENIED)
    parent_manage = ctx(perm.PERMISSION_PLUGIN_MANAGE, parent)
    new_instance = api.CreateInstance(r.RuntimeCreateInstanceRequest(context=parent_manage,
        installation_id=installation, externally_managed=True), timeout=8).instance
    require(new_instance.installation_id == installation, 'exact parent instance creation')
    rejected(api.GetInstance, r.RuntimeGetInstanceRequest(context=parent_manage,
        resource=ref(p.RESOURCE_INSTANCE, new_instance.instance_id)), grpc.StatusCode.PERMISSION_DENIED)
    rejected(api.CreateInstance, r.RuntimeCreateInstanceRequest(context=manage_plugins,
        installation_id=installation, externally_managed=True), grpc.StatusCode.PERMISSION_DENIED)
