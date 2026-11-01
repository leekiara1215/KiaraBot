"""Identity projection fixture rules; production adapters must enforce equivalent invariants."""


def basis_permissions(assignment, role_capabilities, known_permissions, explicit_only):
    if not assignment.scope.resources:
        raise ValueError('empty assignment scope is never global')
    refs = [(ref.kind, ref.id) for ref in assignment.scope.resources]
    if any(kind not in range(1, 17) or not resource for kind, resource in refs) or len(refs) != len(set(refs)):
        raise ValueError('invalid exact assignment resources')
    if assignment.role:
        if assignment.role not in role_capabilities or assignment.permissions:
            raise ValueError('role and explicit permission bases cannot be mixed')
        return set(role_capabilities[assignment.role]) - set(explicit_only)
    values = list(assignment.permissions)
    if not values or len(values) != len(set(values)) or any(value not in known_permissions or value == 0 for value in values):
        raise ValueError('invalid explicit permission basis')
    return set(values)


def assignment_permissions(assignment, role_capabilities, known_permissions, explicit_only):
    if not assignment.assignment_id or not assignment.principal_id or assignment.version <= 0:
        raise ValueError('missing authoritative assignment identity/version')
    return basis_permissions(assignment, role_capabilities, known_permissions, explicit_only)


def assignment_command_valid(request, role_capabilities, known_permissions, explicit_only):
    if not request.assignment.principal_id:
        return False
    try:
        basis_permissions(request.assignment, role_capabilities, known_permissions, explicit_only)
    except ValueError:
        return False
    return (bool(request.assignment_id) and request.HasField('expected_version') and request.expected_version > 0
            or not request.assignment_id and not request.HasField('expected_version'))


def allows(assignments, resource, permission, role_capabilities, known_permissions, explicit_only):
    for assignment in assignments:
        capabilities = assignment_permissions(assignment, role_capabilities, known_permissions, explicit_only)
        if any(ref.kind == resource.kind and ref.id == resource.id for ref in assignment.scope.resources):
            if permission in capabilities:
                return True
    return False


def validate_current_projection(response, role_capabilities, known_permissions, explicit_only):
    if response.authorization_version <= 0 or not response.HasField('checked_at') or response.checked_at.seconds <= 0:
        raise ValueError('missing authoritative current authorization version/time')
    if not response.principal.principal_id or response.principal.version <= 0:
        raise ValueError('missing authoritative principal identity/version')
    if 'session' in response.DESCRIPTOR.fields_by_name:
        if (not response.HasField('session') or not response.session.session_id
                or response.session.principal_id != response.principal.principal_id
                or response.principal.principal_kind != 'human'
                or session_state(response.session, response.checked_at) != 'ACTIVE'):
            raise ValueError('current browser session does not match active human principal')
    capabilities = set()
    roles = set()
    for assignment in response.assignments:
        if assignment.principal_id != response.principal.principal_id:
            raise ValueError('projection includes another principal bindings')
        capabilities.update(assignment_permissions(assignment, role_capabilities, known_permissions, explicit_only))
        if assignment.role:
            roles.add(assignment.role)
    if set(response.principal.roles) != roles or set(response.principal.display_permissions) != capabilities:
        raise ValueError('display summary does not match checked assignment bindings')


def session_state(session, checked_at):
    if not session.HasField('created_at') or not session.HasField('expires_at') or session.rotation <= 0:
        raise ValueError('missing authoritative session fields')
    if session.created_at.seconds <= 0 or checked_at.seconds <= 0:
        raise ValueError('placeholder Identity timestamp')
    if session.expires_at.ToNanoseconds() <= session.created_at.ToNanoseconds():
        raise ValueError('invalid session time interval')
    if session.revoked:
        return 'REVOKED'
    return 'EXPIRED' if checked_at.ToNanoseconds() >= session.expires_at.ToNanoseconds() else 'ACTIVE'


def access_seconds_remaining(tokens, checked_at):
    if not tokens.HasField('access_expires_at') or checked_at.seconds <= 0:
        raise ValueError('missing Identity token check time')
    return max(0, (tokens.access_expires_at.ToNanoseconds() - checked_at.ToNanoseconds()) // 1_000_000_000)
