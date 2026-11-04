"""Regression for reference provenance and independent target production claims."""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from evidence_scopes import EvidenceFailure, EvidenceResolver, effective_status, validate_scopes
from check_implementation_status import implementation_text


def git_blob(content):
    import hashlib
    return hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()


class ScopeTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='kiarabot-contract-evidence-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.logical = 'core/src/application/action_router.cpp'
        self.reference = 'core/reference/src/application/action_router.cpp'
        self.content = b'const char* legacy = "message.send";\n'
        self.manifest = {'base': 'd20b54bcc4ea01df023acb54cd528b4a0764b3db', 'files': [
            {'source': self.logical, 'reference': self.reference, 'blob': git_blob(self.content)}]}
        self.registry = {'categories': {'operations': {'old': 'implemented', 'new': 'not_implemented',
                                                     'removed': 'deprecated'}},
                         'execution_scopes': {
                             'reference': {'status_source': 'categories', 'production': False},
                             'target': {'default_status': 'not_implemented', 'overrides': {}, 'evidence': {}}}}

    def write(self, path, data):
        path = self.root / path
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        return path

    def migrated(self, manifest=None):
        self.write(self.reference, self.content)
        self.write('core/reference/manifest.json', json.dumps(manifest or self.manifest).encode())

    def test_reference_evidence_before_and_after_exact_manifest_migration(self):
        original = self.write(self.logical, self.content)
        self.assertEqual(EvidenceResolver(self.root).resolve(self.logical), original)
        original.unlink()
        self.migrated()
        resolver = EvidenceResolver(self.root)
        self.assertEqual(resolver.resolve(self.logical), self.root / self.reference)
        self.assertFalse(resolver.is_target_source(self.logical))
        self.assertFalse(resolver.is_target_source(self.reference))

    def test_missing_or_wrong_mapping_never_guesses_reference_file(self):
        self.write(self.reference, self.content)
        with self.assertRaisesRegex(EvidenceFailure, 'file missing'):
            EvidenceResolver(self.root).resolve(self.logical)
        wrong = copy.deepcopy(self.manifest)
        wrong['files'][0]['reference'] = 'core/reference/wrong.cpp'
        self.migrated(wrong)
        with self.assertRaisesRegex(EvidenceFailure, 'incorrect reference mapping'):
            EvidenceResolver(self.root)

    def test_manifest_missing_file_changed_blob_duplicate_and_escape_fail(self):
        self.migrated()
        target = self.root / self.reference
        target.unlink()
        with self.assertRaisesRegex(EvidenceFailure, 'destination missing'):
            EvidenceResolver(self.root)
        target.write_bytes(b'changed')
        with self.assertRaisesRegex(EvidenceFailure, 'blob changed'):
            EvidenceResolver(self.root)
        target.write_bytes(self.content)
        for key, value in [('source', '../outside'), ('reference', '../outside'), ('extra', 'unknown')]:
            bad = copy.deepcopy(self.manifest)
            bad['files'][0][key] = value
            self.migrated(bad)
            with self.assertRaises(EvidenceFailure):
                EvidenceResolver(self.root)
        bad = copy.deepcopy(self.manifest)
        bad['files'].append(copy.deepcopy(bad['files'][0]))
        self.migrated(bad)
        with self.assertRaisesRegex(EvidenceFailure, 'duplicate'):
            EvidenceResolver(self.root)

    def test_symlinks_and_reintroduced_source_fail(self):
        self.migrated()
        self.write(self.logical, self.content)
        with self.assertRaisesRegex(EvidenceFailure, 'reintroduced'):
            EvidenceResolver(self.root)
        (self.root / self.logical).unlink()
        (self.root / self.reference).unlink()
        elsewhere = self.write('elsewhere.cpp', self.content)
        (self.root / self.reference).symlink_to(elsewhere)
        with self.assertRaisesRegex(EvidenceFailure, 'symlink'):
            EvidenceResolver(self.root)

    def test_reference_implemented_is_preserved_but_never_implicitly_target(self):
        before = copy.deepcopy(self.registry['categories'])
        validate_scopes(self.registry, EvidenceResolver(self.root))
        self.assertEqual(effective_status(self.registry, 'reference', 'operations', 'old'), 'implemented')
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'old'), 'not_implemented')
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'removed'), 'deprecated')
        self.assertEqual(before, self.registry['categories'])

    def target_claim(self, paths):
        target = self.registry['execution_scopes']['target']
        target['overrides'] = {'operations': {'new': 'implemented'}}
        target['evidence'] = {'operations:new': paths}

    def test_target_claim_requires_current_production_source_and_target_test(self):
        self.migrated()
        test = 'tests/architecture/run.py'
        production = 'core/src/runtime/platform/service_process.cpp'
        self.write(test, b'# target integration assertions\n')
        self.write(production, b'// current production boundary\n')
        for paths in ([self.logical, test], [production], [test]):
            self.target_claim(paths)
            with self.assertRaises(EvidenceFailure):
                validate_scopes(self.registry, EvidenceResolver(self.root))
        self.target_claim([production, test])
        validate_scopes(self.registry, EvidenceResolver(self.root))
        self.assertEqual(effective_status(self.registry, 'target', 'operations', 'new'), 'implemented')
        self.target_claim([production, test, self.logical])
        with self.assertRaisesRegex(EvidenceFailure, 'cannot certify target'):
            validate_scopes(self.registry, EvidenceResolver(self.root))

    def test_unknown_scope_item_and_production_defaults_fail_closed(self):
        resolver = EvidenceResolver(self.root)
        for mutate in [lambda r: r['execution_scopes']['reference'].update(production=True),
                       lambda r: r['execution_scopes']['target'].update(default_status='implemented'),
                       lambda r: r['execution_scopes']['target'].update(overrides={'unknown': {}}),
                       lambda r: r['execution_scopes']['target'].update(overrides={'operations': {'unknown': 'implemented'}}),
                       lambda r: r['execution_scopes']['target'].update(overrides={'operations': {'removed': 'implemented'}})]:
            broken = copy.deepcopy(self.registry)
            mutate(broken)
            with self.assertRaises(EvidenceFailure):
                validate_scopes(broken, resolver)
        with self.assertRaises(EvidenceFailure):
            effective_status(self.registry, 'unknown', 'operations', 'old')

    def test_reference_vocabulary_and_test_strings_do_not_certify_target(self):
        self.migrated()
        self.write('core/tests/platform/example.py', b'feature_test_only')
        self.write('core/src/runtime/platform/example.cpp', b'feature_current_boundary')
        self.assertIn('message.send', implementation_text('reference', self.root))
        current = implementation_text('target', self.root)
        self.assertNotIn('message.send', current)
        self.assertNotIn('feature_test_only', current)
        self.assertIn('feature_current_boundary', current)

    def test_real_registry_only_probes_are_currently_available_in_target(self):
        root = Path(__file__).resolve().parents[2]
        registry = json.loads((root / 'proto/tests/contract_status.json').read_text())
        validate_scopes(registry, EvidenceResolver(root))
        available = {(category, key) for category, values in registry['categories'].items() for key in values
                     if effective_status(registry, 'target', category, key) == 'implemented'}
        expected = {('operations', 'operations.getProcessLiveness'),
                    ('operations', 'operations.getProcessReadiness'),
                    ('operations', 'operations.listAuditEntries'),
                    ('operations', 'operations.listLogs'),
                    ('operations', 'operations.beginDeleteResource'),
                    ('operations', 'operations.restoreResource'),
                    ('operations', 'operations.getOccurrence'),
                    ('proto_messages', 'CredentialDelivery'),
                    ('proto_messages', 'CredentialStatus'),
                    ('proto_messages', 'IdentityGetCredentialStatusRequest'),
                    ('proto_messages', 'IdentityGetCredentialStatusResponse'),
                    ('proto_messages', 'IdentityGetPrincipalStatusRequest'),
                    ('proto_messages', 'IdentityGetPrincipalStatusResponse'),
                    ('proto_messages', 'IdentityRegisterPrincipalRequest'),
                    ('proto_messages', 'IdentityRegisterPrincipalResponse'),
                    ('proto_messages', 'IdentityRevokeCredentialRequest'),
                    ('proto_messages', 'IdentityRevokeCredentialResponse'),
                    ('proto_messages', 'IdentityRotateCredentialRequest'),
                    ('proto_messages', 'IdentityRotateCredentialResponse')}
        expected.add(('proto_messages', 'HostRegisterHostResponse'))
        host_bridge_fields = {
            'HostRegisterHostRequest': 'host',
            'HostRegisterHostResponse': 'host fence',
            'HostRecord': 'host_id version capabilities observed_at',
            'HostToRuntime': 'registration fence heartbeat',
            'RuntimeToHost': 'registered fence heartbeat_ack',
            'HostHeartbeat': 'sent_at',
            'HostHeartbeatAck': 'fence received_at heartbeat_interval_seconds',
        }
        for message, fields in host_bridge_fields.items():
            expected.update(('proto_fields', message + '.' + field) for field in fields.split())
        expected |= {('proto_fields', 'CredentialDelivery.credential_id'), ('proto_fields', 'CredentialDelivery.version'), ('proto_fields', 'CredentialDelivery.new_secret')}
        expected |= {('proto_fields', 'CredentialDelivery.delivery_receipt_id'), ('proto_fields', 'CredentialDelivery.expires_at'), ('proto_fields', 'CredentialStatus.credential_id')}
        expected |= {('proto_fields', 'CredentialStatus.principal_id'), ('proto_fields', 'CredentialStatus.binding'), ('proto_fields', 'CredentialStatus.version')}
        expected |= {('proto_fields', 'CredentialStatus.state'), ('proto_fields', 'CredentialStatus.created_at'), ('proto_fields', 'CredentialStatus.expires_at')}
        expected |= {('proto_fields', 'CredentialStatus.checked_at'), ('proto_fields', 'IdentityGetCredentialStatusRequest.context'), ('proto_fields', 'IdentityGetCredentialStatusRequest.credential_id')}
        expected |= {('proto_fields', 'IdentityGetCredentialStatusResponse.credential'), ('proto_fields', 'IdentityGetPrincipalStatusRequest.context'), ('proto_fields', 'IdentityGetPrincipalStatusRequest.principal_id')}
        expected |= {('proto_fields', 'IdentityGetPrincipalStatusResponse.principal'), ('proto_fields', 'IdentityRegisterPrincipalRequest.context'), ('proto_fields', 'IdentityRegisterPrincipalRequest.principal_kind')}
        expected |= {('proto_fields', 'IdentityRegisterPrincipalRequest.binding'), ('proto_fields', 'IdentityRegisterPrincipalRequest.allowed_services'), ('proto_fields', 'IdentityRegisterPrincipalResponse.principal')}
        expected |= {('proto_fields', 'IdentityRegisterPrincipalResponse.initial_credential'), ('proto_fields', 'IdentityRevokeCredentialRequest.context'), ('proto_fields', 'IdentityRevokeCredentialRequest.credential_id')}
        expected |= {('proto_fields', 'IdentityRevokeCredentialRequest.expected_version'), ('proto_fields', 'IdentityRevokeCredentialResponse.version'), ('proto_fields', 'IdentityRotateCredentialRequest.context')}
        expected |= {('proto_fields', 'IdentityRotateCredentialRequest.credential_id'), ('proto_fields', 'IdentityRotateCredentialRequest.expected_version'), ('proto_fields', 'IdentityRotateCredentialResponse.delivery')}
        expected |= {('proto_messages', 'IdentityAuditFilter'), ('proto_messages', 'IdentityAuditRecord'), ('proto_messages', 'IdentityListAuditRequest')}
        expected |= {('proto_messages', 'IdentityListAuditResponse'), ('proto_messages', 'IdentityListLogsRequest'), ('proto_messages', 'IdentityListLogsResponse')}
        expected |= {('proto_messages', 'IdentityLogRecord'), ('proto_messages', 'RuntimeAuditFilter'), ('proto_messages', 'RuntimeAuditRecord')}
        expected |= {('proto_messages', 'RuntimeListAuditRequest'), ('proto_messages', 'RuntimeListAuditResponse'), ('proto_messages', 'RuntimeListLogsRequest')}
        expected |= {('proto_messages', 'RuntimeListLogsResponse'), ('proto_messages', 'RuntimeLogRecord'), ('proto_fields', 'IdentityAuditFilter.scope')}
        expected |= {('proto_fields', 'IdentityAuditFilter.occurred_from'), ('proto_fields', 'IdentityAuditFilter.occurred_until'), ('proto_fields', 'IdentityAuditFilter.actor')}
        expected |= {('proto_fields', 'IdentityAuditFilter.resource'), ('proto_fields', 'IdentityAuditFilter.page'), ('proto_fields', 'IdentityAuditRecord.entry_id')}
        expected |= {('proto_fields', 'IdentityAuditRecord.actor'), ('proto_fields', 'IdentityAuditRecord.resource'), ('proto_fields', 'IdentityAuditRecord.request_id')}
        expected |= {('proto_fields', 'IdentityAuditRecord.trace_id'), ('proto_fields', 'IdentityAuditRecord.method'), ('proto_fields', 'IdentityAuditRecord.rejection')}
        expected |= {('proto_fields', 'IdentityAuditRecord.authorization_version'), ('proto_fields', 'IdentityAuditRecord.occurred_at'), ('proto_fields', 'IdentityAuditRecord.target_account_id')}
        expected |= {('proto_fields', 'IdentityAuditRecord.target_conversation_id'), ('proto_fields', 'IdentityListAuditRequest.context'), ('proto_fields', 'IdentityListAuditRequest.filter')}
        expected |= {('proto_fields', 'IdentityListAuditResponse.entries'), ('proto_fields', 'IdentityListAuditResponse.page'), ('proto_fields', 'IdentityListAuditResponse.checked_at')}
        expected |= {('proto_fields', 'IdentityListLogsRequest.context'), ('proto_fields', 'IdentityListLogsRequest.filter'), ('proto_fields', 'IdentityListLogsResponse.entries')}
        expected |= {('proto_fields', 'IdentityListLogsResponse.page'), ('proto_fields', 'IdentityListLogsResponse.checked_at'), ('proto_fields', 'IdentityLogRecord.entry_id')}
        expected |= {('proto_fields', 'IdentityLogRecord.severity'), ('proto_fields', 'IdentityLogRecord.component'), ('proto_fields', 'IdentityLogRecord.method')}
        expected |= {('proto_fields', 'IdentityLogRecord.actor'), ('proto_fields', 'IdentityLogRecord.resource'), ('proto_fields', 'IdentityLogRecord.request_id')}
        expected |= {('proto_fields', 'IdentityLogRecord.trace_id'), ('proto_fields', 'IdentityLogRecord.occurred_at'), ('proto_fields', 'IdentityLogRecord.message')}
        expected |= {('proto_fields', 'RuntimeAuditFilter.scope'), ('proto_fields', 'RuntimeAuditFilter.occurred_from'), ('proto_fields', 'RuntimeAuditFilter.occurred_until')}
        expected |= {('proto_fields', 'RuntimeAuditFilter.actor'), ('proto_fields', 'RuntimeAuditFilter.resource'), ('proto_fields', 'RuntimeAuditFilter.page')}
        expected |= {('proto_fields', 'RuntimeAuditRecord.entry_id'), ('proto_fields', 'RuntimeAuditRecord.actor'), ('proto_fields', 'RuntimeAuditRecord.resource')}
        expected |= {('proto_fields', 'RuntimeAuditRecord.request_id'), ('proto_fields', 'RuntimeAuditRecord.trace_id'), ('proto_fields', 'RuntimeAuditRecord.method')}
        expected |= {('proto_fields', 'RuntimeAuditRecord.rejection'), ('proto_fields', 'RuntimeAuditRecord.authorization_version'), ('proto_fields', 'RuntimeAuditRecord.occurred_at')}
        expected |= {('proto_fields', 'RuntimeAuditRecord.target_account_id'), ('proto_fields', 'RuntimeAuditRecord.target_conversation_id'), ('proto_fields', 'RuntimeListAuditRequest.context')}
        expected |= {('proto_fields', 'RuntimeListAuditRequest.filter'), ('proto_fields', 'RuntimeListAuditResponse.entries'), ('proto_fields', 'RuntimeListAuditResponse.page')}
        expected |= {('proto_fields', 'RuntimeListAuditResponse.checked_at'), ('proto_fields', 'RuntimeListLogsRequest.context'), ('proto_fields', 'RuntimeListLogsRequest.filter')}
        expected |= {('proto_fields', 'RuntimeListLogsResponse.entries'), ('proto_fields', 'RuntimeListLogsResponse.page'), ('proto_fields', 'RuntimeListLogsResponse.checked_at')}
        expected |= {('proto_fields', 'RuntimeLogRecord.entry_id'), ('proto_fields', 'RuntimeLogRecord.severity'), ('proto_fields', 'RuntimeLogRecord.component')}
        expected |= {('proto_fields', 'RuntimeLogRecord.method'), ('proto_fields', 'RuntimeLogRecord.actor'), ('proto_fields', 'RuntimeLogRecord.resource')}
        expected |= {('proto_fields', 'RuntimeLogRecord.request_id'), ('proto_fields', 'RuntimeLogRecord.trace_id'), ('proto_fields', 'RuntimeLogRecord.occurred_at')}
        expected |= {('proto_fields', 'RuntimeLogRecord.message')}
        expected |= {('proto_messages', 'ModelChatRequest'), ('proto_messages', 'ModelChatResponse'),
                    ('proto_messages', 'ModelMessage'), ('proto_fields', 'HostRequest.model_chat'),
                    ('proto_fields', 'HostResponse.model_chat'), ('proto_fields', 'ModelChatRequest.model'),
                    ('proto_fields', 'ModelChatRequest.logical_call_id'), ('proto_fields', 'ModelChatRequest.messages'),
                    ('proto_fields', 'ModelChatResponse.text'), ('proto_fields', 'ModelMessage.role'),
                    ('proto_fields', 'ModelMessage.text'), ('proto_fields', 'ModelChatResponse.output_tokens'),
                    ('proto_fields', 'ModelChatResponse.input_tokens'), ('host_operations', 'HostRequest.model_chat'),
                    ('host_results', 'HostResponse.model_chat')}
        expected |= {('proto_fields', 'IdentityWatchInvalidationsResponse.resync_required'),
                    ('proto_fields', 'IdentityWatchInvalidationsResponse.cursor')}
        # Merged P07/P09 target flips: reviewed in their module gates and evidence registry.
        expected |= {
            ('proto_fields', 'ConnectionAuthorization.authorization_version'),
            ('proto_fields', 'ConnectionAuthorization.credential_version'),
            ('proto_fields', 'ConnectionAuthorization.expires_at'),
            ('proto_fields', 'ConnectionAuthorization.fence'),
            ('proto_fields', 'ConnectionAuthorization.issued_at'),
            ('proto_fields', 'ConnectionAuthorization.principal_id'),
            ('proto_fields', 'ConnectionAuthorization.proof'),
            ('proto_fields', 'ConnectionAuthorization.refresh_credential'),
            ('proto_fields', 'ConnectionAuthorization.refresh_generation'),
            ('proto_fields', 'ConnectionAuthorizationResult.authorization'),
            ('proto_fields', 'ConnectionAuthorizationResult.error'),
            ('proto_fields', 'ConnectionAuthorizationResult.request_id'),
            ('proto_fields', 'DeliveryAttempt.attempt_id'),
            ('proto_fields', 'DeliveryAttempt.attempt_number'),
            ('proto_fields', 'DeliveryAttempt.event_id'),
            ('proto_fields', 'DeliveryAttempt.fence'),
            ('proto_fields', 'DeliveryAttempt.lease_until'),
            ('proto_fields', 'DeliveryAttempt.started_at'),
            ('proto_fields', 'DeliveryReceipt.attempt'),
            ('proto_fields', 'DeliveryReceipt.disposition'),
            ('proto_fields', 'DeliveryReceipt.failure'),
            ('proto_fields', 'HostRequest.storage_compare_and_set'),
            ('proto_fields', 'HostResponse.storage_compare_and_set'),
            ('proto_fields', 'NamespaceScope.instance_private'),
            ('proto_fields', 'RefreshConnectionAuthorization.fence'),
            ('proto_fields', 'RefreshConnectionAuthorization.proof'),
            ('proto_fields', 'RefreshConnectionAuthorization.refresh_credential'),
            ('proto_fields', 'RefreshConnectionAuthorization.refresh_generation'),
            ('proto_fields', 'RefreshConnectionAuthorization.request_id'),
            ('proto_fields', 'StorageCompareAndSetRequest.condition'),
            ('proto_fields', 'StorageCompareAndSetRequest.expires_at'),
            ('proto_fields', 'StorageCompareAndSetRequest.key'),
            ('proto_fields', 'StorageCompareAndSetRequest.scope'),
            ('proto_fields', 'StorageCompareAndSetRequest.value'),
            ('proto_fields', 'StorageCompareAndSetResponse.entry'),
            ('proto_fields', 'StorageCondition.absent'),
            ('proto_fields', 'StorageCondition.version'),
            ('proto_fields', 'StorageDeleteRequest.condition'),
            ('proto_fields', 'StorageDeleteRequest.scope'),
            ('proto_fields', 'StorageDeleteResponse.deleted'),
            ('proto_fields', 'StorageDeleteResponse.version'),
            ('proto_fields', 'StorageEntry.expires_at'),
            ('proto_fields', 'StorageEntry.key'),
            ('proto_fields', 'StorageEntry.value'),
            ('proto_fields', 'StorageEntry.version'),
            ('proto_fields', 'StorageGetRequest.scope'),
            ('proto_fields', 'StorageGetResponse.expires_at'),
            ('proto_fields', 'StorageGetResponse.version'),
            ('proto_fields', 'StorageSetRequest.condition'),
            ('proto_fields', 'StorageSetRequest.scope'),
            ('proto_fields', 'StorageSetResponse.expires_at'),
            ('proto_fields', 'StorageSetResponse.version'),
            ('proto_messages', 'StorageCompareAndSetRequest'),
            ('proto_messages', 'StorageCompareAndSetResponse'),
            ('proto_messages', 'StorageCondition'),
            ('proto_messages', 'StorageEntry'),
            ('semantic_rules', 'semantic.namespace.shared_membership'),
            ('semantic_rules', 'semantic.storage.cas_condition'),
            ('semantic_rules', 'semantic.storage.quota_persistent'),
            ('semantic_rules', 'semantic.storage.receipt_capacity'),
            ('semantic_rules', 'semantic.storage.scope_resolution')
        }
        # P11 Model target flips: reviewed via model RPC/application/provider evidence.
        expected |= {
            ('enum_values', 'ModelCallDisposition.ABANDONED'),
            ('enum_values', 'ModelCallDisposition.EXECUTED_BILLED'),
            ('enum_values', 'ModelCallDisposition.EXECUTED_NOT_BILLED'),
            ('enum_values', 'ModelCallDisposition.MODEL_CALL_DISPOSITION_UNSPECIFIED'),
            ('enum_values', 'ModelCallDisposition.NOT_EXECUTED'),
            ('enum_values', 'ModelChatDisposition.FRESH_RESULT'),
            ('enum_values', 'ModelChatDisposition.MODEL_CHAT_DISPOSITION_UNSPECIFIED'),
            ('enum_values', 'ModelChatDisposition.REPLAY_IN_PROGRESS'),
            ('enum_values', 'ModelChatDisposition.REPLAY_TERMINAL'),
            ('enum_values', 'Permission.PERMISSION_MODEL_CALL_INSPECT'),
            ('enum_values', 'Permission.PERMISSION_MODEL_CHAT'),
            ('enum_values', 'Permission.PERMISSION_MODEL_MANAGE'),
            ('enum_values', 'Permission.PERMISSION_MODEL_READ'),
            ('enum_values', 'ResourceKind.RESOURCE_MODEL'),
            ('proto_fields', 'ModelCallRecord.config_version'),
            ('proto_fields', 'ModelCallRecord.disposition'),
            ('proto_fields', 'ModelCallRecord.execution_outcome'),
            ('proto_fields', 'ModelCallRecord.input_tokens'),
            ('proto_fields', 'ModelCallRecord.latency_ms'),
            ('proto_fields', 'ModelCallRecord.logical_call_id'),
            ('proto_fields', 'ModelCallRecord.may_have_billed'),
            ('proto_fields', 'ModelCallRecord.model_id'),
            ('proto_fields', 'ModelCallRecord.observed_at'),
            ('proto_fields', 'ModelCallRecord.output_tokens'),
            ('proto_fields', 'ModelCallRecord.owner_instance_id'),
            ('proto_fields', 'ModelCallRecord.provider_request_id'),
            ('proto_fields', 'ModelCallRecord.reason'),
            ('proto_fields', 'ModelCallRecord.secret_version'),
            ('proto_fields', 'ModelCallRecord.state'),
            ('proto_fields', 'ModelCallRecord.usage_known'),
            ('proto_fields', 'ModelConfiguration.alias'),
            ('proto_fields', 'ModelConfiguration.endpoint'),
            ('proto_fields', 'ModelConfiguration.max_concurrency'),
            ('proto_fields', 'ModelConfiguration.model_id'),
            ('proto_fields', 'ModelConfiguration.provider_kind'),
            ('proto_fields', 'ModelConfiguration.provider_model'),
            ('proto_fields', 'ModelConfiguration.state'),
            ('proto_fields', 'ModelConfiguration.timeout_ms'),
            ('proto_fields', 'ModelConfiguration.version'),
            ('proto_fields', 'ModelDisableModelRequest.context'),
            ('proto_fields', 'ModelDisableModelRequest.expected_version'),
            ('proto_fields', 'ModelDisableModelRequest.model_id'),
            ('proto_fields', 'ModelDisableModelResponse.model'),
            ('proto_fields', 'ModelEnableModelRequest.context'),
            ('proto_fields', 'ModelEnableModelRequest.expected_version'),
            ('proto_fields', 'ModelEnableModelRequest.model_id'),
            ('proto_fields', 'ModelEnableModelResponse.model'),
            ('proto_fields', 'ModelGatewayChatRequest.context'),
            ('proto_fields', 'ModelGatewayChatRequest.logical_call_id'),
            ('proto_fields', 'ModelGatewayChatRequest.messages'),
            ('proto_fields', 'ModelGatewayChatRequest.model_id'),
            ('proto_fields', 'ModelGatewayChatResponse.call'),
            ('proto_fields', 'ModelGatewayChatResponse.disposition'),
            ('proto_fields', 'ModelGatewayChatResponse.result'),
            ('proto_fields', 'ModelGetCallStatusRequest.context'),
            ('proto_fields', 'ModelGetCallStatusRequest.logical_call_id'),
            ('proto_fields', 'ModelGetCallStatusRequest.scope'),
            ('proto_fields', 'ModelGetCallStatusResponse.call'),
            ('proto_fields', 'ModelGetModelConfigurationRequest.context'),
            ('proto_fields', 'ModelGetModelConfigurationRequest.model_id'),
            ('proto_fields', 'ModelGetModelConfigurationRequest.scope'),
            ('proto_fields', 'ModelGetModelConfigurationResponse.config'),
            ('proto_fields', 'ModelGetModelConfigurationResponse.observed_at'),
            ('proto_fields', 'ModelGetModelConfigurationResponse.secret_configured'),
            ('proto_fields', 'ModelGetModelConfigurationResponse.secret_version'),
            ('proto_fields', 'ModelGetModelMetadataRequest.context'),
            ('proto_fields', 'ModelGetModelMetadataRequest.model_id'),
            ('proto_fields', 'ModelGetModelMetadataRequest.scope'),
            ('proto_fields', 'ModelGetModelMetadataResponse.model'),
            ('proto_fields', 'ModelGetOperationRequest.context'),
            ('proto_fields', 'ModelGetOperationRequest.operation'),
            ('proto_fields', 'ModelGetOperationResponse.operation'),
            ('proto_fields', 'ModelListModelMetadataRequest.context'),
            ('proto_fields', 'ModelListModelMetadataRequest.page'),
            ('proto_fields', 'ModelListModelMetadataRequest.scope'),
            ('proto_fields', 'ModelListModelMetadataResponse.models'),
            ('proto_fields', 'ModelListModelMetadataResponse.page'),
            ('proto_fields', 'ModelMetadata.alias'),
            ('proto_fields', 'ModelMetadata.config_version'),
            ('proto_fields', 'ModelMetadata.model_id'),
            ('proto_fields', 'ModelMetadata.provider_kind'),
            ('proto_fields', 'ModelMetadata.state'),
            ('proto_fields', 'ModelResolveModelCallRequest.context'),
            ('proto_fields', 'ModelResolveModelCallRequest.disposition'),
            ('proto_fields', 'ModelResolveModelCallRequest.expected_version'),
            ('proto_fields', 'ModelResolveModelCallRequest.logical_call_id'),
            ('proto_fields', 'ModelResolveModelCallResponse.call'),
            ('proto_fields', 'ModelRotateProviderSecretRequest.context'),
            ('proto_fields', 'ModelRotateProviderSecretRequest.expected_version'),
            ('proto_fields', 'ModelRotateProviderSecretRequest.model_id'),
            ('proto_fields', 'ModelRotateProviderSecretRequest.new_secret'),
            ('proto_fields', 'ModelRotateProviderSecretResponse.operation'),
            ('proto_fields', 'ModelRotateProviderSecretResponse.version'),
            ('proto_fields', 'ModelUpdateModelRequest.config'),
            ('proto_fields', 'ModelUpdateModelRequest.context'),
            ('proto_fields', 'ModelUpdateModelRequest.expected_version'),
            ('proto_fields', 'ModelUpdateModelResponse.model'),
            ('proto_fields', 'ModelUpdateModelResponse.operation'),
            ('proto_messages', 'ModelCallRecord'),
            ('proto_messages', 'ModelConfiguration'),
            ('proto_messages', 'ModelDisableModelRequest'),
            ('proto_messages', 'ModelDisableModelResponse'),
            ('proto_messages', 'ModelEnableModelRequest'),
            ('proto_messages', 'ModelEnableModelResponse'),
            ('proto_messages', 'ModelGatewayChatRequest'),
            ('proto_messages', 'ModelGatewayChatResponse'),
            ('proto_messages', 'ModelGetCallStatusRequest'),
            ('proto_messages', 'ModelGetCallStatusResponse'),
            ('proto_messages', 'ModelGetModelConfigurationRequest'),
            ('proto_messages', 'ModelGetModelConfigurationResponse'),
            ('proto_messages', 'ModelGetModelMetadataRequest'),
            ('proto_messages', 'ModelGetModelMetadataResponse'),
            ('proto_messages', 'ModelGetOperationRequest'),
            ('proto_messages', 'ModelGetOperationResponse'),
            ('proto_messages', 'ModelListModelMetadataRequest'),
            ('proto_messages', 'ModelListModelMetadataResponse'),
            ('proto_messages', 'ModelMetadata'),
            ('proto_messages', 'ModelResolveModelCallRequest'),
            ('proto_messages', 'ModelResolveModelCallResponse'),
            ('proto_messages', 'ModelRotateProviderSecretRequest'),
            ('proto_messages', 'ModelRotateProviderSecretResponse'),
            ('proto_messages', 'ModelUpdateModelRequest'),
            ('proto_messages', 'ModelUpdateModelResponse'),
            ('semantic_rules', 'semantic.model.duplicate_call_replay'),
            ('semantic_rules', 'semantic.model.instance_delegation'),
            ('semantic_rules', 'semantic.model.provider_error_mapping'),
            ('semantic_rules', 'semantic.model.request_hash'),
            ('semantic_rules', 'semantic.model.secret_rotation_pinning'),
            ('semantic_rules', 'semantic.model.two_phase_unknown'),
            ('semantic_rules', 'semantic.model.usage_known_billing')
        }
        # P11d-2 Control Model REST target flips: real Control-process TLS e2e evidence.
        expected |= {
            ('operations', 'operations.disableModel'),
            ('operations', 'operations.enableModel'),
            ('operations', 'operations.getModelCallStatus'),
            ('operations', 'operations.getModelConfiguration'),
            ('operations', 'operations.getModelMetadata'),
            ('operations', 'operations.listModels'),
            ('operations', 'operations.resolveModelCall'),
            ('operations', 'operations.rotateProviderSecret'),
            ('operations', 'operations.updateModel')
        }
        # P06 external Bridge: both audience-bound Connect streams, bootstrap ->
        # ConnectionAuthorization/refresh, delivery attempt/lease/receipt and shutdown.
        expected |= {('bridge_methods', 'AdapterBridge.Connect'),
                     ('bridge_methods', 'PluginBridge.Connect')}
        expected |= {('event_payloads', 'EventEnvelope.message_received')}
        expected |= {('semantic_rules', 'semantic.bridge.connection_authorization'),
                     ('semantic_rules', 'semantic.bridge.external_listener'),
                     ('semantic_rules', 'semantic.bridge.plugin_delivery_lease')}
        expected |= {('enum_values', 'DeliveryDisposition.DELIVERY_ACKNOWLEDGED'),
                     ('enum_values', 'DeliveryDisposition.DELIVERY_DUPLICATE'),
                     ('enum_values', 'DeliveryDisposition.DELIVERY_STALE'),
                     ('enum_values', 'DeliveryDisposition.DELIVERY_RENEWED'),
                     ('enum_values', 'DeliveryDisposition.DELIVERY_REJECTED')}
        expected |= {('proto_fields', 'RegisterAdapterRequest.adapter_instance_id'),
                     ('proto_fields', 'RegisterAdapterResponse.fence'),
                     ('proto_fields', 'RegisterAdapterResponse.effective_capabilities'),
                     ('proto_fields', 'RegisterAdapterResponse.authorization'),
                     ('proto_fields', 'AdapterToCore.fence'),
                     ('proto_fields', 'AdapterToCore.connection_proof'),
                     ('proto_fields', 'AdapterToCore.refresh_authorization'),
                     ('proto_fields', 'CoreToAdapter.fence'),
                     ('proto_fields', 'CoreToAdapter.authorization_result'),
                     ('proto_fields', 'RegisterPluginRequest.installation_id'),
                     ('proto_fields', 'RegisterPluginRequest.execution_generation'),
                     ('proto_fields', 'RegisterPluginResponse.fence'),
                     ('proto_fields', 'RegisterPluginResponse.desired_config_version'),
                     ('proto_fields', 'RegisterPluginResponse.authorization'),
                     ('proto_fields', 'PluginToCore.fence'),
                     ('proto_fields', 'PluginToCore.connection_proof'),
                     ('proto_fields', 'PluginToCore.renew_delivery'),
                     ('proto_fields', 'PluginToCore.refresh_authorization'),
                     ('proto_fields', 'CoreToPlugin.fence'),
                     ('proto_fields', 'CoreToPlugin.delivery_attempt'),
                     ('proto_fields', 'CoreToPlugin.delivery_receipt'),
                     ('proto_fields', 'CoreToPlugin.authorization_result'),
                     ('proto_fields', 'PluginEventAck.attempt')}
        expected |= {('proto_fields', 'CoreToAdapter.shutdown'),
                     ('proto_fields', 'CoreToPlugin.shutdown'),
                     ('proto_fields', 'ShutdownNotice.reason'),
                     ('proto_fields', 'ShutdownNotice.drain_timeout_ms'),
                     ('semantic_rules', 'semantic.transport.graceful_shutdown')}
        expected |= {('proto_fields', name)
                     for name in ('DeliveryAttempt.event_id', 'DeliveryAttempt.attempt_id',
                                  'DeliveryAttempt.fence', 'DeliveryAttempt.started_at',
                                  'DeliveryAttempt.lease_until', 'DeliveryAttempt.attempt_number',
                                  'DeliveryReceipt.attempt', 'DeliveryReceipt.disposition',
                                  'DeliveryReceipt.failure')}
        expected |= {('proto_fields', name)
                     for name in ('ConnectionAuthorization.fence', 'ConnectionAuthorization.proof',
                                  'ConnectionAuthorization.issued_at',
                                  'ConnectionAuthorization.expires_at',
                                  'ConnectionAuthorization.refresh_credential',
                                  'ConnectionAuthorization.refresh_generation',
                                  'ConnectionAuthorization.principal_id',
                                  'ConnectionAuthorization.credential_version',
                                  'ConnectionAuthorization.authorization_version',
                                  'RefreshConnectionAuthorization.request_id',
                                  'RefreshConnectionAuthorization.fence',
                                  'RefreshConnectionAuthorization.proof',
                                  'RefreshConnectionAuthorization.refresh_credential',
                                  'RefreshConnectionAuthorization.refresh_generation',
                                  'ConnectionAuthorizationResult.request_id',
                                  'ConnectionAuthorizationResult.authorization',
                                  'ConnectionAuthorizationResult.error')}
        expected |= {
            ('proto_messages', 'ActionEnvelope'),
            ('proto_messages', 'ActionResult'),
            ('proto_messages', 'PluginActionCancel'),
            ('proto_messages', 'PluginActionQuery'),
            ('proto_messages', 'PluginActionRequest'),
            ('proto_messages', 'PluginActionResponse'),
            ('proto_messages', 'ActionMetadata'),
            ('proto_messages', 'ActionExecutionEvidence'),
            ('proto_messages', 'ActionReceipt'),
            ('proto_messages', 'RuntimeQueryActionResultRequest'),
            ('proto_messages', 'RuntimeQueryActionResultResponse'),
            ('proto_fields', 'ActionEnvelope.action_id'),
            ('proto_fields', 'ActionEnvelope.deadline'),
            ('proto_fields', 'ActionEnvelope.idempotency_key'),
            ('proto_fields', 'ActionEnvelope.send_message'),
            ('proto_fields', 'ActionEnvelope.target'),
            ('proto_fields', 'ActionEnvelope.target_source'),
            ('proto_fields', 'ActionEnvelope.trace_id'),
            ('proto_fields', 'ActionResult.completed_at'),
            ('proto_fields', 'ActionResult.error'),
            ('proto_fields', 'ActionResult.platform_message_id'),
            ('proto_fields', 'ActionResult.status'),
            ('proto_fields', 'AdapterToCore.action_result'),
            ('proto_fields', 'CoreToAdapter.action'),
            ('proto_fields', 'CoreToPlugin.action_response'),
            ('proto_fields', 'PluginActionCancel.action_id'),
            ('proto_fields', 'PluginActionCancel.deadline'),
            ('proto_fields', 'PluginActionCancel.request_id'),
            ('proto_fields', 'PluginActionCancel.trace_id'),
            ('proto_fields', 'PluginActionQuery.action_id'),
            ('proto_fields', 'PluginActionQuery.deadline'),
            ('proto_fields', 'PluginActionQuery.idempotency_key'),
            ('proto_fields', 'PluginActionQuery.request_id'),
            ('proto_fields', 'PluginActionQuery.trace_id'),
            ('proto_fields', 'PluginActionRequest.action'),
            ('proto_fields', 'PluginActionRequest.request_id'),
            ('proto_fields', 'PluginActionResponse.error'),
            ('proto_fields', 'PluginActionResponse.request_id'),
            ('proto_fields', 'PluginActionResponse.result'),
            ('proto_fields', 'PluginToCore.action'),
            ('proto_fields', 'PluginToCore.action_cancel'),
            ('proto_fields', 'PluginToCore.action_query'),
            ('proto_fields', 'ActionMetadata.action_id'),
            ('proto_fields', 'ActionMetadata.owner_instance_id'),
            ('proto_fields', 'ActionMetadata.account_id'),
            ('proto_fields', 'ActionMetadata.status'),
            ('proto_fields', 'ActionMetadata.execution_outcome'),
            ('proto_fields', 'ActionMetadata.updated_at'),
            ('proto_fields', 'ActionEnvelope.dispatch_attempt_id'),
            ('proto_fields', 'ActionEnvelope.dispatch_sequence'),
            ('proto_fields', 'ActionResult.execution_outcome'),
            ('proto_fields', 'ActionResult.dispatch_attempt_id'),
            ('proto_fields', 'AdapterToCore.action_evidence'),
            ('proto_fields', 'CoreToAdapter.action_receipt'),
            ('proto_fields', 'ActionExecutionEvidence.action_id'),
            ('proto_fields', 'ActionExecutionEvidence.dispatch_attempt_id'),
            ('proto_fields', 'ActionExecutionEvidence.execution_outcome'),
            ('proto_fields', 'ActionExecutionEvidence.observed_at'),
            ('proto_fields', 'ActionExecutionEvidence.last_error'),
            ('proto_fields', 'ActionExecutionEvidence.platform_reference'),
            ('proto_fields', 'ActionReceipt.action_id'),
            ('proto_fields', 'ActionReceipt.dispatch_attempt_id'),
            ('proto_fields', 'ActionReceipt.disposition'),
            ('proto_fields', 'ActionReceipt.error'),
            ('proto_fields', 'ActionReceipt.result'),
            ('proto_fields', 'ActionMetadata.kind'),
            ('proto_fields', 'ActionMetadata.target_account_id'),
            ('proto_fields', 'ActionMetadata.target_conversation_id'),
            ('proto_fields', 'ActionMetadata.created_at'),
            ('proto_fields', 'ActionMetadata.deadline'),
            ('proto_fields', 'ActionMetadata.retain_until'),
            ('proto_fields', 'ActionMetadata.idempotency_key'),
            ('proto_fields', 'ActionMetadata.platform_message_id'),
            ('proto_fields', 'ActionMetadata.dispatch_attempt_id'),
            ('proto_fields', 'ActionMetadata.error'),
            ('proto_fields', 'RuntimeQueryActionResultRequest.context'),
            ('proto_fields', 'RuntimeQueryActionResultRequest.account_scope'),
            ('proto_fields', 'RuntimeQueryActionResultRequest.action_id'),
            ('proto_fields', 'RuntimeQueryActionResultRequest.idempotency_key'),
            ('proto_fields', 'RuntimeQueryActionResultRequest.owner_instance_id'),
            ('proto_fields', 'RuntimeQueryActionResultResponse.result'),
            ('proto_fields', 'RuntimeQueryActionResultResponse.execution_outcome'),
            ('proto_fields', 'RuntimeQueryActionResultResponse.metadata'),
            ('action_payloads', 'ActionEnvelope.send_message'),
            ('enum_values', 'ActionReceiptDisposition.ACTION_RECEIPT_RECORDED'),
            ('enum_values', 'ActionReceiptDisposition.ACTION_RECEIPT_DUPLICATE'),
            ('enum_values', 'ActionReceiptDisposition.ACTION_RECEIPT_EVIDENCE_ONLY'),
            ('enum_values', 'Permission.PERMISSION_MESSAGE_SEND'),
            ('enum_values', 'Permission.PERMISSION_MESSAGE_REACT'),
            ('enum_values', 'Permission.PERMISSION_MESSAGE_DELETE'),
            ('enum_values', 'Permission.PERMISSION_MEMBER_MUTE'),
            ('enum_values', 'Permission.PERMISSION_MEMBER_KICK'),
            ('enum_values', 'Permission.PERMISSION_REQUEST_APPROVE'),
            ('semantic_rules', 'semantic.action.deadline_max_300s'),
            ('semantic_rules', 'semantic.action.result_execution_outcome'),
            ('semantic_rules', 'semantic.action.dispatch_attempt_correlation'),
            ('semantic_rules', 'semantic.action.evidence_late_result'),
            ('semantic_rules', 'semantic.action.quota_persistence'),
        }
        expected |= {
            ('proto_messages', 'RuntimeSetSharedMembersResponse'),
            ('proto_messages', 'RuntimeCreateSharedNamespaceResponse'),
            ('proto_fields', 'RuntimeSetSharedMembersRequest.installation_id'),
            ('proto_fields', 'RuntimeSetSharedMembersRequest.namespace_id'),
            ('proto_fields', 'RuntimeGetSharedNamespaceResponse.namespace'),
            ('proto_fields', 'RuntimeSetSharedMembersRequest.members'),
            ('proto_fields', 'RuntimeCreateSharedNamespaceRequest.installation_id'),
            ('proto_fields', 'RuntimeDisableSharedNamespaceRequest.context'),
            ('proto_fields', 'RuntimeDisableSharedNamespaceRequest.namespace_id'),
            ('proto_fields', 'StorageQuota.installation_bytes_limit'),
            ('proto_messages', 'RuntimeGetSharedNamespaceResponse'),
            ('proto_fields', 'NamespaceRecord.created_at'),
            ('proto_fields', 'StorageQuota.namespace_bytes_used'),
            ('proto_fields', 'StorageQuota.installation_keys_used'),
            ('proto_fields', 'SharedMember.can_write'),
            ('proto_messages', 'RuntimeSetSharedMembersRequest'),
            ('proto_fields', 'RuntimeCreateSharedNamespaceRequest.expected_version'),
            ('proto_messages', 'RuntimeDisableSharedNamespaceRequest'),
            ('proto_fields', 'NamespaceRecord.updated_at'),
            ('proto_fields', 'RuntimeGetSharedNamespaceRequest.namespace_id'),
            ('proto_messages', 'RuntimeCreateSharedNamespaceRequest'),
            ('proto_fields', 'RuntimeCreateSharedNamespaceResponse.namespace'),
            ('proto_fields', 'RuntimeCreateSharedNamespaceRequest.context'),
            ('proto_messages', 'RuntimeDisableSharedNamespaceResponse'),
            ('proto_fields', 'RuntimeGetSharedNamespaceResponse.members'),
            ('proto_fields', 'RuntimeDisableSharedNamespaceResponse.operation'),
            ('proto_fields', 'NamespaceScope.installation_shared'),
            ('proto_messages', 'RuntimeGetSharedNamespaceRequest'),
            ('proto_fields', 'NamespaceRecord.installation_id'),
            ('proto_fields', 'StorageQuota.installation_keys_limit'),
            ('proto_fields', 'NamespaceRecord.version'),
            ('proto_fields', 'StorageQuota.namespace_keys_used'),
            ('proto_fields', 'RuntimeSetSharedMembersResponse.version'),
            ('proto_fields', 'RuntimeDisableSharedNamespaceRequest.expected_version'),
            ('proto_fields', 'SharedMember.can_read'),
            ('proto_fields', 'RuntimeGetSharedNamespaceResponse.quota'),
            ('proto_fields', 'StorageQuota.namespace_keys_limit'),
            ('proto_messages', 'SharedMember'),
            ('proto_fields', 'RuntimeDisableSharedNamespaceResponse.version'),
            ('proto_fields', 'RuntimeSetSharedMembersRequest.expected_version'),
            ('proto_messages', 'NamespaceRecord'),
            ('proto_fields', 'RuntimeSetSharedMembersRequest.context'),
            ('proto_fields', 'StorageQuota.namespace_bytes_limit'),
            ('proto_fields', 'NamespaceRecord.disabled'),
            ('proto_fields', 'StorageQuota.installation_bytes_used'),
            ('proto_fields', 'NamespaceRecord.namespace_id'),
            ('proto_fields', 'SharedMember.instance_id'),
            ('proto_messages', 'StorageQuota'),
            ('proto_fields', 'RuntimeGetSharedNamespaceRequest.context'),
            ('proto_fields', 'RuntimeSetSharedMembersResponse.operation'),
        }
        expected |= {
            ('proto_messages', 'RuntimeGetOperationRequest'),
            ('proto_messages', 'RuntimeGetOperationResponse'),
            ('proto_messages', 'RuntimeBeginDeleteRequest'),
            ('proto_messages', 'RuntimeBeginDeleteResponse'),
            ('proto_messages', 'RuntimeRestoreResourceRequest'),
            ('proto_messages', 'RuntimeRestoreResourceResponse'),
            ('proto_messages', 'OperationRef'),
            ('proto_messages', 'OperationProgress'),
            ('proto_messages', 'Operation'),
            ('proto_fields', 'RuntimeGetOperationRequest.context'),
            ('proto_fields', 'RuntimeGetOperationRequest.operation'),
            ('proto_fields', 'RuntimeGetOperationResponse.operation'),
            ('proto_fields', 'RuntimeBeginDeleteRequest.context'),
            ('proto_fields', 'RuntimeBeginDeleteRequest.resource'),
            ('proto_fields', 'RuntimeBeginDeleteRequest.expected_version'),
            ('proto_fields', 'RuntimeBeginDeleteResponse.operation'),
            ('proto_fields', 'RuntimeRestoreResourceRequest.context'),
            ('proto_fields', 'RuntimeRestoreResourceRequest.resource'),
            ('proto_fields', 'RuntimeRestoreResourceRequest.expected_version'),
            ('proto_fields', 'RuntimeRestoreResourceResponse.operation'),
            ('proto_fields', 'OperationRef.owner_service'),
            ('proto_fields', 'OperationRef.operation_id'),
            ('proto_fields', 'OperationRef.resource'),
            ('proto_fields', 'OperationProgress.participant'),
            ('proto_fields', 'OperationProgress.state'),
            ('proto_fields', 'OperationProgress.reason'),
            ('proto_fields', 'OperationProgress.next_attempt_at'),
            ('proto_fields', 'Operation.ref'),
            ('proto_fields', 'Operation.version'),
            ('proto_fields', 'Operation.state'),
            ('proto_fields', 'Operation.execution_outcome'),
            ('proto_fields', 'Operation.participants'),
            ('proto_fields', 'Operation.observed_at'),
            ('proto_fields', 'Operation.reason'),
            ('proto_fields', 'Operation.next_attempt_at'),
            ('proto_fields', 'Operation.stalled'),
            ('enum_values', 'OperationState.OPERATION_STATE_UNSPECIFIED'),
            ('enum_values', 'OperationState.OPERATION_ACCEPTED'),
            ('enum_values', 'OperationState.OPERATION_RUNNING'),
            ('enum_values', 'OperationState.OPERATION_BLOCKED'),
            ('enum_values', 'OperationState.OPERATION_SUCCEEDED'),
            ('enum_values', 'OperationState.OPERATION_FAILED'),
            ('enum_values', 'OperationState.OPERATION_CANCELLED'),
            ('enum_values', 'OperationState.OPERATION_EXPIRED'),
            ('operations', 'operations.getOperation'),
        }
        expected |= {
            ('proto_messages', 'ModelChatRequest'),
            ('proto_messages', 'ModelMessage'),
            ('proto_messages', 'ModelChatResponse'),
            ('proto_fields', 'HostRequest.model_chat'),
            ('proto_fields', 'HostResponse.model_chat'),
            ('proto_fields', 'ModelChatResponse.text'),
            ('proto_fields', 'ModelChatRequest.model'),
            ('proto_fields', 'ModelChatRequest.messages'),
            ('proto_fields', 'ModelChatRequest.logical_call_id'),
            ('proto_fields', 'ModelMessage.role'),
            ('proto_fields', 'ModelMessage.text'),
            ('proto_fields', 'ModelChatResponse.input_tokens'),
            ('proto_fields', 'ModelChatResponse.output_tokens'),
            ('host_operations', 'HostRequest.model_chat'),
            ('host_results', 'HostResponse.model_chat'),
        }
        # P10g Scheduler target flips + P12c/P13 operation probes already registered in main.
        expected |= {
            ('proto_messages', 'TaskRecord'),
            ('proto_messages', 'OccurrenceRecord'),
            ('proto_messages', 'SchedulerCreateTaskRequest'),
            ('proto_messages', 'SchedulerCreateTaskResponse'),
            ('proto_messages', 'SchedulerCancelTaskRequest'),
            ('proto_messages', 'SchedulerCancelTaskResponse'),
            ('proto_messages', 'SchedulerGetTaskRequest'),
            ('proto_messages', 'SchedulerGetTaskResponse'),
            ('proto_messages', 'SchedulerListTasksRequest'),
            ('proto_messages', 'SchedulerListTasksResponse'),
            ('proto_messages', 'SchedulerGetOccurrenceRequest'),
            ('proto_messages', 'SchedulerGetOccurrenceResponse'),
            ('proto_messages', 'SchedulerGetOperationRequest'),
            ('proto_messages', 'SchedulerGetOperationResponse'),
            ('proto_fields', 'TaskRecord.task_id'),
            ('proto_fields', 'TaskRecord.owner_instance_id'),
            ('proto_fields', 'TaskRecord.installation_id'),
            ('proto_fields', 'TaskRecord.target'),
            ('proto_fields', 'TaskRecord.payload'),
            ('proto_fields', 'TaskRecord.run_at'),
            ('proto_fields', 'TaskRecord.state'),
            ('proto_fields', 'TaskRecord.version'),
            ('proto_fields', 'TaskRecord.occurrence_id'),
            ('proto_fields', 'OccurrenceRecord.occurrence_id'),
            ('proto_fields', 'OccurrenceRecord.task_id'),
            ('proto_fields', 'OccurrenceRecord.state'),
            ('proto_fields', 'OccurrenceRecord.handoff'),
            ('proto_fields', 'OccurrenceRecord.accepted_event_id'),
            ('proto_fields', 'OccurrenceRecord.acknowledged_at'),
            ('proto_fields', 'SchedulerCreateTaskRequest.context'),
            ('proto_fields', 'SchedulerCreateTaskRequest.task_id'),
            ('proto_fields', 'SchedulerCreateTaskRequest.target'),
            ('proto_fields', 'SchedulerCreateTaskRequest.payload'),
            ('proto_fields', 'SchedulerCreateTaskRequest.run_at'),
            ('proto_fields', 'SchedulerCreateTaskRequest.local_time'),
            ('proto_fields', 'SchedulerCreateTaskResponse.task'),
            ('proto_fields', 'SchedulerCancelTaskRequest.context'),
            ('proto_fields', 'SchedulerCancelTaskRequest.task_id'),
            ('proto_fields', 'SchedulerCancelTaskRequest.scope'),
            ('proto_fields', 'SchedulerCancelTaskResponse.task'),
            ('proto_fields', 'SchedulerGetTaskRequest.context'),
            ('proto_fields', 'SchedulerGetTaskRequest.task_id'),
            ('proto_fields', 'SchedulerGetTaskRequest.scope'),
            ('proto_fields', 'SchedulerGetTaskResponse.task'),
            ('proto_fields', 'SchedulerListTasksRequest.context'),
            ('proto_fields', 'SchedulerListTasksRequest.scope'),
            ('proto_fields', 'SchedulerListTasksRequest.page'),
            ('proto_fields', 'SchedulerListTasksResponse.tasks'),
            ('proto_fields', 'SchedulerListTasksResponse.page'),
            ('proto_fields', 'SchedulerGetOccurrenceRequest.context'),
            ('proto_fields', 'SchedulerGetOccurrenceRequest.occurrence_id'),
            ('proto_fields', 'SchedulerGetOccurrenceRequest.scope'),
            ('proto_fields', 'SchedulerGetOccurrenceResponse.occurrence'),
            ('proto_fields', 'SchedulerGetOperationRequest.context'),
            ('proto_fields', 'SchedulerGetOperationRequest.operation'),
            ('proto_fields', 'SchedulerGetOperationResponse.operation'),
            ('proto_fields', 'TaskRecord.created_at'),
            ('proto_fields', 'TaskRecord.updated_at'),
            ('proto_fields', 'TaskRecord.cancel_requested'),
            ('proto_fields', 'TaskRecord.reason'),
            ('proto_fields', 'OccurrenceRecord.attempts'),
            ('proto_fields', 'OccurrenceRecord.last_error'),
            ('proto_fields', 'OccurrenceRecord.created_at'),
            ('proto_fields', 'OccurrenceRecord.updated_at'),
            ('proto_fields', 'SchedulerListTasksRequest.state'),
            ('proto_fields', 'TimerTriggered.occurrence_id'),
            ('semantic_rules', 'semantic.scheduler.owner_derivation'),
            ('semantic_rules', 'semantic.scheduler.cancel_window'),
            ('semantic_rules', 'semantic.scheduler.handoff_expiry'),
            ('semantic_rules', 'semantic.scheduler.timer_occurrence_identity'),
            ('operations', 'operations.listAuditEntries'),
            ('operations', 'operations.listLogs'),
            ('operations', 'operations.beginDeleteResource'),
            ('operations', 'operations.restoreResource'),
            ('operations', 'operations.getTask'),
            ('operations', 'operations.cancelTask'),
        }
        # P07f-2 target flips: Action payload wire + ActionPolicy owner RPC/policy evidence.
        expected |= {
            ('action_payloads', 'ActionEnvelope.approve_request'),
            ('action_payloads', 'ActionEnvelope.delete_message'),
            ('action_payloads', 'ActionEnvelope.kick_member'),
            ('action_payloads', 'ActionEnvelope.mute_member'),
            ('action_payloads', 'ActionEnvelope.react_message'),
            ('operations', 'operations.getActionPolicy'),
            ('operations', 'operations.updateActionPolicy'),
            ('proto_fields', 'ActionPolicy.account_id'),
            ('proto_fields', 'ActionPolicy.allowed_group_ids'),
            ('proto_fields', 'ActionPolicy.kick_daily_budget'),
            ('proto_fields', 'ActionPolicy.kick_enabled'),
            ('proto_fields', 'ActionPolicy.mute_daily_budget'),
            ('proto_fields', 'ActionPolicy.mute_enabled'),
            ('proto_fields', 'ActionPolicy.version'),
            ('proto_fields', 'ApproveRequest.approve'),
            ('proto_fields', 'ApproveRequest.reason'),
            ('proto_fields', 'ApproveRequest.request_id'),
            ('proto_fields', 'DeleteMessage.conversation'),
            ('proto_fields', 'DeleteMessage.message_id'),
            ('proto_fields', 'KickMember.group_id'),
            ('proto_fields', 'KickMember.reason'),
            ('proto_fields', 'KickMember.user_id'),
            ('proto_fields', 'MuteMember.duration_seconds'),
            ('proto_fields', 'MuteMember.group_id'),
            ('proto_fields', 'MuteMember.user_id'),
            ('proto_fields', 'ReactMessage.conversation'),
            ('proto_fields', 'ReactMessage.message_id'),
            ('proto_fields', 'ReactMessage.reaction'),
            ('proto_fields', 'RuntimeGetActionPolicyRequest.account_id'),
            ('proto_fields', 'RuntimeGetActionPolicyRequest.context'),
            ('proto_fields', 'RuntimeGetActionPolicyResponse.policy'),
            ('proto_fields', 'RuntimeUpdateActionPolicyRequest.context'),
            ('proto_fields', 'RuntimeUpdateActionPolicyRequest.expected_version'),
            ('proto_fields', 'RuntimeUpdateActionPolicyRequest.policy'),
            ('proto_fields', 'RuntimeUpdateActionPolicyResponse.policy'),
            ('proto_messages', 'ActionPolicy'),
            ('proto_messages', 'ApproveRequest'),
            ('proto_messages', 'DeleteMessage'),
            ('proto_messages', 'KickMember'),
            ('proto_messages', 'MuteMember'),
            ('proto_messages', 'ReactMessage'),
            ('proto_messages', 'RuntimeGetActionPolicyRequest'),
            ('proto_messages', 'RuntimeGetActionPolicyResponse'),
            ('proto_messages', 'RuntimeUpdateActionPolicyRequest'),
            ('proto_messages', 'RuntimeUpdateActionPolicyResponse')
        }
        expected |= {
            ('proto_messages', 'AuthorizationDecision'),
            ('proto_messages', 'DelegationProof'),
            ('proto_messages', 'IdentityCheckAuthorizationRequest'),
            ('proto_messages', 'IdentityCheckAuthorizationResponse'),
            ('proto_messages', 'IdentityExchangeDelegationRequest'),
            ('proto_messages', 'IdentityExchangeDelegationResponse'),
            ('proto_messages', 'IdentityGetVerificationKeysRequest'),
            ('proto_messages', 'IdentityGetVerificationKeysResponse'),
            ('proto_messages', 'IdentityIssueDelegationRequest'),
            ('proto_messages', 'IdentityIssueDelegationResponse'),
            ('proto_messages', 'VerificationKey'),
            ('proto_fields', 'AuthorizationDecision.allowed'),
            ('proto_fields', 'AuthorizationDecision.authorization_version'),
            ('proto_fields', 'AuthorizationDecision.checked_at'),
            ('proto_fields', 'AuthorizationDecision.denial'),
            ('proto_fields', 'DelegationProof.audience'),
            ('proto_fields', 'DelegationProof.business_hops'),
            ('proto_fields', 'DelegationProof.expires_at'),
            ('proto_fields', 'DelegationProof.proof'),
            ('proto_fields', 'IdentityCheckAuthorizationRequest.context'),
            ('proto_fields', 'IdentityCheckAuthorizationRequest.full_method'),
            ('proto_fields', 'IdentityCheckAuthorizationRequest.known_authorization_version'),
            ('proto_fields', 'IdentityCheckAuthorizationRequest.scope'),
            ('proto_fields', 'IdentityCheckAuthorizationRequest.subject_proof'),
            ('proto_fields', 'IdentityCheckAuthorizationResponse.decision'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.audience'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.client_certificate_sha256'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.context'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.parent_proof'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.permissions'),
            ('proto_fields', 'IdentityExchangeDelegationRequest.scope'),
            ('proto_fields', 'IdentityExchangeDelegationResponse.delegation'),
            ('proto_fields', 'IdentityGetVerificationKeysRequest.context'),
            ('proto_fields', 'IdentityGetVerificationKeysRequest.known_key_version'),
            ('proto_fields', 'IdentityGetVerificationKeysResponse.keys'),
            ('proto_fields', 'IdentityGetVerificationKeysResponse.version'),
            ('proto_fields', 'IdentityIssueDelegationRequest.audience'),
            ('proto_fields', 'IdentityIssueDelegationRequest.client_certificate_sha256'),
            ('proto_fields', 'IdentityIssueDelegationRequest.context'),
            ('proto_fields', 'IdentityIssueDelegationRequest.instance_id'),
            ('proto_fields', 'IdentityIssueDelegationRequest.permissions'),
            ('proto_fields', 'IdentityIssueDelegationRequest.scope'),
            ('proto_fields', 'IdentityIssueDelegationRequest.subject_proof'),
            ('proto_fields', 'IdentityIssueDelegationResponse.delegation'),
            ('proto_fields', 'VerificationKey.ed25519_public_key'),
            ('proto_fields', 'VerificationKey.key_id'),
            ('proto_fields', 'VerificationKey.valid_until'),
            ('proto_fields', 'VerificationKey.version'),
        }
        expected |= {
            ('proto_messages', 'IdentityPrincipalSummary'),
            ('proto_messages', 'IdentityListPrincipalsRequest'),
            ('proto_messages', 'IdentityListPrincipalsResponse'),
            ('proto_fields', 'IdentityPrincipalSummary.principal_id'),
            ('proto_fields', 'IdentityPrincipalSummary.binding'),
            ('proto_fields', 'IdentityPrincipalSummary.principal_kind'),
            ('proto_fields', 'IdentityPrincipalSummary.version'),
            ('proto_fields', 'IdentityPrincipalSummary.state'),
            ('proto_fields', 'IdentityPrincipalSummary.roles'),
            ('proto_fields', 'IdentityListPrincipalsRequest.context'),
            ('proto_fields', 'IdentityListPrincipalsRequest.page'),
            ('proto_fields', 'IdentityListPrincipalsRequest.state_filter'),
            ('proto_fields', 'IdentityListPrincipalsResponse.principals'),
            ('proto_fields', 'IdentityListPrincipalsResponse.page'),
            ('proto_fields', 'IdentityListPrincipalsResponse.checked_at'),
            ('operations', 'operations.listPrincipals')
        }
        session_messages = {
            'IdentityLoginRequest': 'context username password',
            'IdentityLoginResponse': 'tokens checked_at',
            'IdentityRefreshSessionRequest': 'context refresh_token',
            'IdentityRefreshSessionResponse': 'tokens checked_at',
            'IdentityValidateSessionRequest': 'context access_token',
            'IdentityValidateSessionResponse': 'principal session assignments authorization_version checked_at',
            'IdentityGetPrincipalRequest': 'context access_token',
            'IdentityGetPrincipalResponse': 'principal assignments authorization_version checked_at',
            'IdentityListSessionsRequest': 'context principal_id page',
            'IdentityListSessionsResponse': 'sessions page checked_at',
            'IdentityRevokeSessionRequest': 'context session_id',
            'IdentityRevokeSessionResponse': 'session checked_at',
            'SessionRecord': 'session_id principal_id rotation expires_at revoked created_at',
            'TokenPair': 'session_id access_token refresh_token access_expires_at refresh_expires_at rotation',
        }
        for message, fields in session_messages.items():
            expected.add(('proto_messages', message))
            expected.update(('proto_fields', message + '.' + field) for field in fields.split())
        expected |= {
            ('proto_messages', 'IdentityListAssignmentsRequest'),
            ('proto_fields', 'IdentityListAssignmentsRequest.context'),
            ('proto_fields', 'IdentityListAssignmentsRequest.principal_id'),
            ('proto_fields', 'IdentityListAssignmentsRequest.scope'),
            ('proto_fields', 'IdentityListAssignmentsRequest.page'),
            ('proto_messages', 'IdentityListAssignmentsResponse'),
            ('proto_fields', 'IdentityListAssignmentsResponse.assignments'),
            ('proto_fields', 'IdentityListAssignmentsResponse.page'),
            ('proto_messages', 'PrincipalRecord'),
            ('proto_fields', 'PrincipalRecord.principal_id'),
            ('proto_fields', 'PrincipalRecord.binding'),
            ('proto_fields', 'PrincipalRecord.principal_kind'),
            ('proto_fields', 'PrincipalRecord.version'),
            ('proto_fields', 'PrincipalRecord.state'),
            ('proto_fields', 'PrincipalRecord.roles'),
            ('proto_fields', 'PrincipalRecord.display_permissions'),
            ('proto_messages', 'AssignmentRecord'),
            ('proto_fields', 'AssignmentRecord.assignment_id'),
            ('proto_fields', 'AssignmentRecord.principal_id'),
            ('proto_fields', 'AssignmentRecord.role'),
            ('proto_fields', 'AssignmentRecord.permissions'),
            ('proto_fields', 'AssignmentRecord.scope'),
            ('proto_fields', 'AssignmentRecord.version'),
            ('proto_messages', 'IdentitySetAssignmentRequest'),
            ('proto_fields', 'IdentitySetAssignmentRequest.context'),
            ('proto_fields', 'IdentitySetAssignmentRequest.assignment'),
            ('proto_fields', 'IdentitySetAssignmentRequest.expected_version'),
            ('proto_fields', 'IdentitySetAssignmentRequest.assignment_id'),
            ('proto_messages', 'IdentitySetAssignmentResponse'),
            ('proto_fields', 'IdentitySetAssignmentResponse.assignment'),
            ('proto_messages', 'AssignmentSpec'),
            ('proto_fields', 'AssignmentSpec.principal_id'),
            ('proto_fields', 'AssignmentSpec.role'),
            ('proto_fields', 'AssignmentSpec.permissions'),
            ('proto_fields', 'AssignmentSpec.scope'),
            ('proto_messages', 'IdentityRevokeAssignmentRequest'),
            ('proto_fields', 'IdentityRevokeAssignmentRequest.context'),
            ('proto_fields', 'IdentityRevokeAssignmentRequest.assignment_id'),
            ('proto_fields', 'IdentityRevokeAssignmentRequest.expected_version'),
            ('proto_messages', 'IdentityRevokeAssignmentResponse'),
            ('proto_fields', 'IdentityRevokeAssignmentResponse.version'),
            ('proto_messages', 'IdentityWatchInvalidationsRequest'),
            ('proto_fields', 'IdentityWatchInvalidationsRequest.context'),
            ('proto_fields', 'IdentityWatchInvalidationsRequest.scope'),
            ('proto_fields', 'IdentityWatchInvalidationsRequest.cursor'),
            ('proto_messages', 'IdentityWatchInvalidationsResponse'),
            ('proto_fields', 'IdentityWatchInvalidationsResponse.invalidation'),
            ('proto_messages', 'InvalidationRecord'),
            ('proto_fields', 'InvalidationRecord.cursor'),
            ('proto_fields', 'InvalidationRecord.principal_id'),
            ('proto_fields', 'InvalidationRecord.authorization_version'),
            ('proto_fields', 'InvalidationRecord.scope'),
            ('proto_messages', 'StreamCursor'),
            ('proto_fields', 'StreamCursor.source_service'),
            ('proto_fields', 'StreamCursor.epoch'),
            ('proto_fields', 'StreamCursor.scope'),
            ('proto_fields', 'StreamCursor.sequence'),
            ('proto_messages', 'IdentityGetAuthorizationSnapshotRequest'),
            ('proto_fields', 'IdentityGetAuthorizationSnapshotRequest.context'),
            ('proto_fields', 'IdentityGetAuthorizationSnapshotRequest.scope'),
            ('proto_fields', 'IdentityGetAuthorizationSnapshotRequest.after'),
            ('proto_messages', 'IdentityGetAuthorizationSnapshotResponse'),
            ('proto_fields', 'IdentityGetAuthorizationSnapshotResponse.snapshot'),
            ('proto_messages', 'AuthorizationSnapshot'),
            ('proto_fields', 'AuthorizationSnapshot.cursor'),
            ('proto_fields', 'AuthorizationSnapshot.assignments'),
            ('proto_fields', 'AuthorizationSnapshot.authorization_version'),
        }
        for message, fields in {
            'RuntimeGetInstallationRequest': 'context resource',
            'RuntimeGetInstallationResponse': 'installation observed_at',
            'InstallationRecord': 'installation_id plugin_type_id manifest version state',
            'RuntimeGetInstanceRequest': 'context resource',
            'RuntimeGetInstanceResponse': 'instance observed_at',
            'InstanceRecord': 'instance_id installation_id externally_managed state',
            'VersionedResourceState': 'resource version lifecycle desired desired_config_version applied_config_version observed_at',
            'RuntimeGetInstanceConfigRequest': 'context resource',
            'RuntimeGetInstanceConfigResponse': 'config',
            'InstanceConfiguration': 'instance desired_version values applied_version apply_state observed_at',
        }.items():
            expected.add(('proto_messages', message))
            expected.update(('proto_fields', f'{message}.{field}') for field in fields.split())
        expected.update({
            ('proto_messages', 'RuntimeGetAccountRequest'),
            ('proto_fields', 'RuntimeGetAccountRequest.context'),
            ('proto_fields', 'RuntimeGetAccountRequest.resource'),
            ('proto_messages', 'RuntimeGetAccountResponse'),
            ('proto_fields', 'RuntimeGetAccountResponse.account'),
            ('proto_fields', 'RuntimeGetAccountResponse.observed_at'),
            ('proto_messages', 'RuntimeGetAdapterRequest'),
            ('proto_fields', 'RuntimeGetAdapterRequest.context'),
            ('proto_fields', 'RuntimeGetAdapterRequest.resource'),
            ('proto_messages', 'RuntimeGetAdapterResponse'),
            ('proto_fields', 'RuntimeGetAdapterResponse.adapter'),
            ('proto_fields', 'RuntimeGetAdapterResponse.observed_at'),
            ('proto_messages', 'RuntimeUpdateAccountRequest'),
            ('proto_fields', 'RuntimeUpdateAccountRequest.context'),
            ('proto_fields', 'RuntimeUpdateAccountRequest.account_id'),
            ('proto_fields', 'RuntimeUpdateAccountRequest.display_name'),
            ('proto_fields', 'RuntimeUpdateAccountRequest.expected_version'),
            ('proto_messages', 'RuntimeUpdateAccountResponse'),
            ('proto_fields', 'RuntimeUpdateAccountResponse.account'),
            ('proto_messages', 'AccountRecord'),
            ('proto_fields', 'AccountRecord.account_id'),
            ('proto_fields', 'AccountRecord.platform'),
            ('proto_fields', 'AccountRecord.platform_account_id'),
            ('proto_fields', 'AccountRecord.display_name'),
            ('proto_fields', 'AccountRecord.version'),
            ('proto_fields', 'AccountRecord.state'),
            ('proto_messages', 'AdapterRecord'),
            ('proto_fields', 'AdapterRecord.resource'),
            ('proto_fields', 'AdapterRecord.version'),
            ('proto_fields', 'AdapterRecord.last_registration'),
            ('proto_fields', 'AdapterRecord.last_registered_at'),
            ('proto_fields', 'AdapterRecord.observed_at'),
            ('proto_fields', 'AdapterRecord.current_fence'),
            ('proto_fields', 'AdapterRecord.lifecycle'),
            ('proto_messages', 'AdapterRegistrationMetadata'),
            ('proto_fields', 'AdapterRegistrationMetadata.implementation'),
            ('proto_fields', 'AdapterRegistrationMetadata.implementation_version'),
            ('proto_fields', 'AdapterRegistrationMetadata.protocol_version'),
        })
        # P05 accepted catalog/creation surfaces. Keep the field allowlist explicit so untested
        # host placement, bootstrap credentials and manifest violations remain unavailable.
        for message, fields in {
            'RuntimeCreateAccountRequest': 'context platform platform_account_id display_name catalog',
            'RuntimeCreateAccountResponse': 'account',
            'RuntimeValidateManifestRequest': 'context manifest catalog',
            'RuntimeValidateManifestResponse': 'valid',
            'RuntimeCreateInstallationRequest': 'context plugin_type_id manifest catalog',
            'RuntimeCreateInstallationResponse': 'installation',
            'RuntimeCreateInstanceRequest': 'context installation_id externally_managed',
            'RuntimeCreateInstanceResponse': 'instance',
            'RuntimeListAccountsRequest': 'context catalog page',
            'RuntimeListAccountsResponse': 'accounts snapshot',
            'RuntimeListInstallationsRequest': 'context catalog page',
            'RuntimeListInstallationsResponse': 'installations snapshot',
            'RuntimeListInstancesRequest': 'context catalog page',
            'RuntimeListInstancesResponse': 'instances snapshot',
            'RuntimeListAdaptersRequest': 'context catalog page',
            'RuntimeListAdaptersResponse': 'adapters snapshot',
            'PageRequest': 'page_size page_token',
            'PageInfo': 'next_page_token page_size',
            'SnapshotInfo': 'snapshot_id observed_at expires_at authorization_version owner_service page',
        }.items():
            expected.add(('proto_messages', message))
            expected.update(('proto_fields', f'{message}.{field}') for field in fields.split())
        expected.update(('operations', 'operations.' + operation) for operation in (
            'listAccounts', 'listInstallations', 'listInstances', 'listAdapterInstances',
            'createAccount', 'createInstallation', 'createInstance',
        ))
        # HERR-02/03: real authenticated PluginBridge raw-wire refusals; no broader Host family.
        expected.update(('semantic_rules', semantic) for semantic in (
            'semantic.host.request_operation_classification',
            'semantic.host.unknown_operation_error',
        ))
        self.assertEqual(available, expected)
        self.assertEqual(effective_status(registry, 'reference', 'action_payloads', 'ActionEnvelope.send_message'), 'implemented')
        self.assertEqual(effective_status(registry, 'target', 'action_payloads', 'ActionEnvelope.send_message'), 'implemented')


if __name__ == '__main__':
    unittest.main()
