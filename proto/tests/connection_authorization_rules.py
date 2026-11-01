"""Deterministic connection-authorization fixture oracle, not a production verifier."""
import hmac


def fence_key(fence):
    if (not fence.instance_id or not fence.runtime_epoch
            or not fence.HasField('execution_generation') or fence.execution_generation <= 0
            or not fence.HasField('connection_generation') or fence.connection_generation <= 0):
        raise ValueError('incomplete fence')
    return (fence.instance_id, fence.execution_generation, fence.connection_generation, fence.runtime_epoch)


def valid_bundle(authorization):
    try:
        fence_key(authorization.fence)
    except ValueError:
        return False
    lifetime = authorization.expires_at.ToNanoseconds() - authorization.issued_at.ToNanoseconds()
    return (authorization.HasField('issued_at') and authorization.HasField('expires_at')
            and authorization.issued_at.seconds > 0 and 0 < lifetime <= 56_000_000_000
            and len(authorization.proof) >= 32 and len(authorization.refresh_credential) >= 32
            and authorization.proof != authorization.refresh_credential
            and authorization.refresh_generation > 0 and bool(authorization.principal_id)
            and authorization.credential_version > 0 and authorization.authorization_version > 0)


def frame_allowed(current, received_fence, proof, stream, trusted_stream, utc_ns, monotonic_ns, monotonic_deadline,
                  revoked=False, gate=True, credential_version=None):
    # Stream identity comes from trusted transport state, never a client field.
    if not valid_bundle(current) or stream != trusted_stream or revoked or not gate:
        return False
    try:
        same_fence = fence_key(current.fence) == fence_key(received_fence)
    except ValueError:
        return False
    return (same_fence and hmac.compare_digest(proof, current.proof)
            and current.issued_at.ToNanoseconds() <= utc_ns < current.expires_at.ToNanoseconds()
            and monotonic_ns < monotonic_deadline
            and (credential_version is None or credential_version == current.credential_version))


def refresh(current, request, replacement, online, allowed, **frame_args):
    if (not request.request_id or not frame_allowed(current, request.fence, request.proof, **frame_args)
            or not hmac.compare_digest(request.refresh_credential, current.refresh_credential)):
        return 'UNAUTHENTICATED'
    if request.refresh_generation != current.refresh_generation:
        return 'CONFLICT'
    if not online:
        return 'UPSTREAM_UNAVAILABLE'
    if not allowed:
        return 'PERMISSION_DENIED'
    if (not valid_bundle(replacement) or fence_key(replacement.fence) != fence_key(current.fence)
            or replacement.refresh_generation != current.refresh_generation + 1
            or replacement.proof == current.proof or replacement.refresh_credential == current.refresh_credential
            or replacement.principal_id != current.principal_id
            or replacement.credential_version != current.credential_version
            or replacement.authorization_version < current.authorization_version):
        return 'INVALID_ARGUMENT'
    current.CopyFrom(replacement)  # Models one atomic trusted connection-state transition.
    return 'OK'


def registration_valid(response):
    return (not response.accepted or (response.HasField('authorization')
            and valid_bundle(response.authorization)
            and response.fence == response.authorization.fence))


def refresh_frame_consistent(frame):
    return (frame.WhichOneof('body') == 'refresh_authorization'
            and frame.fence == frame.refresh_authorization.fence
            and bool(frame.connection_proof)
            and hmac.compare_digest(frame.connection_proof, frame.refresh_authorization.proof))
