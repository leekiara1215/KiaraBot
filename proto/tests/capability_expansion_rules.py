"""Approved contract oracles only; none is target producer/consumer evidence."""
from google.protobuf.unknown_fields import UnknownFieldSet


def host_operation_classification(request):
    """Classify only parsed operation shape, after transport/auth/envelope validation.

    Known oneof tags use protobuf last-one-wins. Their duplicate history is unavailable.
    Other operation payload validation remains the operation owner's responsibility.
    """
    candidates = [field for field in UnknownFieldSet(request)
                  if 10 <= field.field_number <= 127]
    known = request.WhichOneof('operation')
    if candidates:
        if known or len(candidates) != 1 or candidates[0].wire_type != 2:
            return 'INVALID_ARGUMENT'
        return 'CAPABILITY_NOT_SUPPORTED'
    return 'KNOWN_OPERATION' if known else 'INVALID_ARGUMENT'


def host_cancel_allowed(*, admission_changed, runtime_gate_requested):
    """Cancellation never undoes a closed gate or already-requested remote effect."""
    return not admission_changed and not runtime_gate_requested
