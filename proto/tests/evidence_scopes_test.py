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
        expected |= {('proto_fields', 'IdentityWatchInvalidationsResponse.resync_required'),
                    ('proto_fields', 'IdentityWatchInvalidationsResponse.cursor')}
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
        self.assertEqual(available, expected)
        self.assertEqual(effective_status(registry, 'reference', 'action_payloads', 'ActionEnvelope.send_message'), 'implemented')
        self.assertEqual(effective_status(registry, 'target', 'action_payloads', 'ActionEnvelope.send_message'), 'implemented')


if __name__ == '__main__':
    unittest.main()
