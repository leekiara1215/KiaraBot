"""Executable query-shape fixtures. Not an authenticated service implementation."""
import re

UUID4 = re.compile(r'[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}')
RESOURCE_KINDS = {'Account': 1, 'Installation': 4, 'Instance': 5, 'Adapter': 2, 'Host': 6}


def runtime_query_valid(request):
    name = request.DESCRIPTOR.name.removeprefix('Runtime').removesuffix('Request')
    if name == 'GetInstanceConfig':
        kind = RESOURCE_KINDS['Instance']
    elif name.startswith('Get'):
        kind = RESOURCE_KINDS.get(name[3:])
    elif name.startswith('List'):
        kind = RESOURCE_KINDS.get(name[4:-1])
    else:
        return False
    if kind is None:
        return False
    field = 'catalog' if name.startswith('List') else 'resource'
    if not request.HasField(field):
        return False
    resource = getattr(request, field)
    if resource.kind != (16 if field == 'catalog' else kind) or not UUID4.fullmatch(resource.id):
        return False
    if field == 'catalog':
        if request.page.page_size > 100 or len(request.page.page_token) > 1024:
            return False
    return True
