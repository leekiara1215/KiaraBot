from kiarabot_sdk import Plugin


class Observer(Plugin):
    """Minimal event plugin. It imports neither Core nor platform modules."""
    def __init__(self):
        self.events = []

    async def on_event(self, event, context):
        # Demonstration state only; real side effects must be idempotent by event_id.
        self.events.append(event.event_id)


def create_plugin():
    return Observer()
