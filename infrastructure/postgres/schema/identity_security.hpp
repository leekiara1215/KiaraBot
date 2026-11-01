#pragma once

#include <string_view>

namespace kiarabot::infrastructure::postgres::schema {

// Identity-owned verification material only. No raw password/token/proof or generic payload store.
inline constexpr std::string_view kIdentitySecurity = R"sql(
CREATE TABLE kiarabot.credential_heads (
  id text PRIMARY KEY CHECK(id<>''), subject text NOT NULL REFERENCES kiarabot.principals(id),
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  leaf_fingerprint bytea UNIQUE CHECK(octet_length(leaf_fingerprint)=32),
  material integer NOT NULL CHECK(material BETWEEN 0 AND 3),
  bootstrap_key text CHECK(bootstrap_key<>''),
  bootstrap_tag bytea CHECK(octet_length(bootstrap_tag)=32),
  UNIQUE(bootstrap_key,bootstrap_tag),
  CHECK((material IN (0,1) AND leaf_fingerprint IS NULL AND bootstrap_key IS NULL AND bootstrap_tag IS NULL) OR
        (material=2 AND leaf_fingerprint IS NULL AND bootstrap_key IS NOT NULL AND bootstrap_tag IS NOT NULL) OR
        (material=3 AND leaf_fingerprint IS NOT NULL AND bootstrap_key IS NULL AND bootstrap_tag IS NULL)),
  UNIQUE(id,subject)
);
CREATE TABLE kiarabot.credential_versions (
  id text NOT NULL, version kiarabot.uint64 NOT NULL CHECK(version>0), subject text NOT NULL,
  material integer NOT NULL CHECK(material BETWEEN 0 AND 3),
  state integer NOT NULL CHECK(state BETWEEN 0 AND 2),
  created_at bigint NOT NULL, revoked_at bigint, expires_at bigint,
  binding_kind integer NOT NULL CHECK(binding_kind BETWEEN 0 AND 15), binding_id text NOT NULL,
  PRIMARY KEY(id,version), UNIQUE(id,version,subject), UNIQUE(id,version,material),
  FOREIGN KEY(id,subject) REFERENCES kiarabot.credential_heads(id,subject)
    DEFERRABLE INITIALLY DEFERRED,
  CHECK(expires_at IS NULL OR expires_at>created_at),
  CHECK((state=0 AND material=0 AND revoked_at IS NULL AND expires_at IS NULL) OR
        (state=1 AND material<>0 AND revoked_at IS NULL AND expires_at IS NOT NULL) OR
        (state=2 AND revoked_at>=created_at AND revoked_at IS NOT NULL))
);
ALTER TABLE kiarabot.credential_heads ADD FOREIGN KEY(id,version)
  REFERENCES kiarabot.credential_versions(id,version) DEFERRABLE INITIALLY DEFERRED;
ALTER TABLE kiarabot.credential_heads ADD FOREIGN KEY(id,version,material)
  REFERENCES kiarabot.credential_versions(id,version,material) DEFERRABLE INITIALLY DEFERRED;
CREATE TABLE kiarabot.credential_pending (
  id text NOT NULL, version kiarabot.uint64 NOT NULL, material integer NOT NULL CHECK(material=0),
  kind integer NOT NULL CHECK(kind BETWEEN 1 AND 3), PRIMARY KEY(id,version),
  FOREIGN KEY(id,version,material) REFERENCES kiarabot.credential_versions(id,version,material)
);
CREATE TABLE kiarabot.credential_passwords (
  id text NOT NULL, version kiarabot.uint64 NOT NULL, material integer NOT NULL CHECK(material=1),
  encoded text NOT NULL CHECK(encoded<>''), hash_policy_version kiarabot.uint64 NOT NULL
    CHECK(hash_policy_version>0), PRIMARY KEY(id,version),
  FOREIGN KEY(id,version,material) REFERENCES kiarabot.credential_versions(id,version,material)
);
CREATE TABLE kiarabot.credential_bootstraps (
  id text NOT NULL, version kiarabot.uint64 NOT NULL, material integer NOT NULL CHECK(material=2),
  verifier_key text NOT NULL CHECK(verifier_key<>''), verifier_tag bytea NOT NULL
    CHECK(octet_length(verifier_tag)=32),
  binding_kind integer NOT NULL CHECK(binding_kind BETWEEN 0 AND 15), binding_id text NOT NULL,
  expires_at bigint NOT NULL, PRIMARY KEY(id,version),
  UNIQUE(id,version,verifier_key,verifier_tag),
  FOREIGN KEY(id,version,material) REFERENCES kiarabot.credential_versions(id,version,material)
);
ALTER TABLE kiarabot.credential_heads ADD FOREIGN KEY(id,version,bootstrap_key,bootstrap_tag)
  REFERENCES kiarabot.credential_bootstraps(id,version,verifier_key,verifier_tag)
    DEFERRABLE INITIALLY DEFERRED;
CREATE TABLE kiarabot.credential_audiences (
  id text NOT NULL, version kiarabot.uint64 NOT NULL,
  position integer NOT NULL CHECK(position>=0),
  audience integer NOT NULL CHECK(audience BETWEEN 0 AND 5), PRIMARY KEY(id,version,position),
  FOREIGN KEY(id,version) REFERENCES kiarabot.credential_bootstraps(id,version)
);
CREATE TABLE kiarabot.credential_certificates (
  id text NOT NULL, version kiarabot.uint64 NOT NULL, material integer NOT NULL CHECK(material=3),
  subject text NOT NULL, fingerprint bytea NOT NULL CHECK(octet_length(fingerprint)=32),
  registered_san text NOT NULL CHECK(registered_san<>''),
  platform_service integer CHECK(platform_service BETWEEN 0 AND 5),
  not_before bigint NOT NULL, not_after bigint NOT NULL CHECK(not_after>not_before),
  PRIMARY KEY(id,version), UNIQUE(id,version,fingerprint),
  FOREIGN KEY(id,version,material) REFERENCES kiarabot.credential_versions(id,version,material),
  FOREIGN KEY(id,version,subject) REFERENCES kiarabot.credential_versions(id,version,subject)
);
ALTER TABLE kiarabot.credential_heads ADD FOREIGN KEY(id,version,leaf_fingerprint)
  REFERENCES kiarabot.credential_certificates(id,version,fingerprint) DEFERRABLE INITIALLY DEFERRED;
CREATE TABLE kiarabot.login_identities (
  normalized_login text PRIMARY KEY CHECK(normalized_login<>''),
  subject text NOT NULL REFERENCES kiarabot.principals(id),
  password_credential text NOT NULL, version kiarabot.uint64 NOT NULL CHECK(version>0),
  FOREIGN KEY(password_credential,subject) REFERENCES kiarabot.credential_heads(id,subject)
);
ALTER TABLE kiarabot.sessions ADD UNIQUE(id,subject,version,rotation,expires_at,revoked);
CREATE TABLE kiarabot.session_security (
  id text PRIMARY KEY, subject text NOT NULL,
  credential_id text NOT NULL, credential_version kiarabot.uint64 NOT NULL,
  access_key text NOT NULL CHECK(access_key<>''), access_tag bytea NOT NULL
    CHECK(octet_length(access_tag)=32),
  refresh_key text NOT NULL CHECK(refresh_key<>''), refresh_tag bytea NOT NULL
    CHECK(octet_length(refresh_tag)=32),
  created_at bigint NOT NULL, access_expires_at bigint NOT NULL, refresh_expires_at bigint NOT NULL,
  rotation kiarabot.uint64 NOT NULL CHECK(rotation>0),
  version kiarabot.uint64 NOT NULL CHECK(version>0), revoked boolean NOT NULL,
  CHECK(access_expires_at>created_at AND refresh_expires_at>=access_expires_at),
  UNIQUE(access_key,access_tag), UNIQUE(refresh_key,refresh_tag),
  FOREIGN KEY(credential_id,credential_version,subject)
    REFERENCES kiarabot.credential_versions(id,version,subject),
  FOREIGN KEY(id,subject,version,rotation,refresh_expires_at,revoked)
    REFERENCES kiarabot.sessions(id,subject,version,rotation,expires_at,revoked) DEFERRABLE INITIALLY DEFERRED
);
CREATE INDEX session_security_subject ON kiarabot.session_security(subject,id);
ALTER TABLE kiarabot.grants ADD UNIQUE(id,subject,version);
CREATE TABLE kiarabot.exact_assignments (
  id text PRIMARY KEY, subject text NOT NULL, version kiarabot.uint64 NOT NULL CHECK(version>0),
  FOREIGN KEY(id,subject,version) REFERENCES kiarabot.grants(id,subject,version)
    DEFERRABLE INITIALLY DEFERRED
);
CREATE INDEX exact_assignments_subject ON kiarabot.exact_assignments(subject,id);
CREATE TABLE kiarabot.exact_roles (
  assignment text NOT NULL REFERENCES kiarabot.exact_assignments(id) ON DELETE CASCADE,
  role integer NOT NULL CHECK(role BETWEEN 0 AND 2),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL,
  PRIMARY KEY(assignment,role,resource_kind,resource_id)
);
CREATE TABLE kiarabot.exact_permissions (
  assignment text NOT NULL REFERENCES kiarabot.exact_assignments(id) ON DELETE CASCADE,
  permission integer NOT NULL CHECK(permission BETWEEN 1 AND 40),
  resource_kind integer NOT NULL CHECK(resource_kind BETWEEN 0 AND 15), resource_id text NOT NULL,
  PRIMARY KEY(assignment,permission,resource_kind,resource_id)
);
CREATE TABLE kiarabot.secret_receipts (
  scope_kind integer NOT NULL CHECK(scope_kind BETWEEN 0 AND 2),
  scope_actor text NOT NULL CHECK(scope_actor<>''), scope_target text NOT NULL CHECK(scope_target<>''),
  idempotency_key text NOT NULL CHECK(idempotency_key<>''),
  subject text NOT NULL REFERENCES kiarabot.principals(id), session text REFERENCES kiarabot.sessions(id),
  credential_id text NOT NULL, credential_version kiarabot.uint64 NOT NULL,
  request_parameter kiarabot.uint64 NOT NULL,
  result_rotation kiarabot.uint64 NOT NULL,
  request_key text NOT NULL CHECK(request_key<>''), request_tag bytea NOT NULL
    CHECK(octet_length(request_tag)=32),
  created_at bigint NOT NULL, retain_until bigint NOT NULL,
  sealed_key text NOT NULL CHECK(sealed_key<>''), format_version bigint NOT NULL
    CHECK(format_version BETWEEN 1 AND 4294967295),
  nonce bytea NOT NULL CHECK(octet_length(nonce)>0),
  ciphertext bytea NOT NULL CHECK(octet_length(ciphertext)>0),
  PRIMARY KEY(scope_kind,scope_actor,scope_target,idempotency_key),
  FOREIGN KEY(credential_id,credential_version,subject)
    REFERENCES kiarabot.credential_versions(id,version,subject),
  CHECK(retain_until>created_at AND retain_until::numeric-created_at::numeric<=86400000000000),
  CHECK((scope_kind=0 AND request_parameter=0 AND session IS NOT NULL AND result_rotation>0) OR
        (scope_kind=1 AND request_parameter>0 AND session=scope_target AND session IS NOT NULL
          AND result_rotation>request_parameter) OR
        (scope_kind=2 AND request_parameter>0 AND credential_id=scope_target AND session IS NULL
          AND result_rotation=0))
);
CREATE INDEX secret_receipts_expiry ON kiarabot.secret_receipts(retain_until);
CREATE TABLE kiarabot.consumed_refresh (
  verifier_key text NOT NULL CHECK(verifier_key<>''), verifier_tag bytea NOT NULL
    CHECK(octet_length(verifier_tag)=32),
  session text NOT NULL REFERENCES kiarabot.sessions(id), subject text NOT NULL,
  credential_id text NOT NULL, credential_version kiarabot.uint64 NOT NULL,
  input_rotation kiarabot.uint64 NOT NULL CHECK(input_rotation>0), input_expires_at bigint NOT NULL,
  scope_kind integer NOT NULL CHECK(scope_kind=1), control_peer text NOT NULL,
  idempotency_key text NOT NULL, consumed_at bigint NOT NULL, recovery_until bigint NOT NULL,
  PRIMARY KEY(verifier_key,verifier_tag), UNIQUE(control_peer,session,idempotency_key),
  CHECK(recovery_until>consumed_at AND recovery_until<=input_expires_at AND
        recovery_until::numeric-consumed_at::numeric<=86400000000000),
  FOREIGN KEY(credential_id,credential_version,subject)
    REFERENCES kiarabot.credential_versions(id,version,subject),
  FOREIGN KEY(scope_kind,control_peer,session,idempotency_key)
    REFERENCES kiarabot.secret_receipts(scope_kind,scope_actor,scope_target,idempotency_key)
    ON DELETE CASCADE DEFERRABLE INITIALLY DEFERRED
);
CREATE INDEX consumed_refresh_expiry ON kiarabot.consumed_refresh(recovery_until);
CREATE TABLE kiarabot.administrator_bootstrap (
  singleton boolean PRIMARY KEY CHECK(singleton), state integer NOT NULL CHECK(state IN (0,1)),
  version kiarabot.uint64 NOT NULL CHECK(version>0),
  administrator text REFERENCES kiarabot.principals(id), consumed_at bigint,
  CHECK((state=0 AND administrator IS NULL AND consumed_at IS NULL) OR
        (state=1 AND administrator IS NOT NULL AND consumed_at IS NOT NULL))
);
INSERT INTO kiarabot.administrator_bootstrap VALUES(true,0,1,NULL,NULL);
)sql";

} // namespace kiarabot::infrastructure::postgres::schema
