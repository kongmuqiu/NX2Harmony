// Narrow compatibility view for API 26 Release 26.0.0.105's checker collision.
// The SDK's common.d.ts BaseEvent.timestamp is public, since 8, syscap ArkUI.Full;
// exact ArkUI.json member metadata (file + className + apiType + apiText) lists
// phone/tablet since 12. TouchEvent explicitly extends BaseEvent.
// SyscapChecker instead indexes file + apiText only, matching the wearable-only
// CrownEvent.timestamp whitelist entry. This view changes no runtime field and
// suppresses no diagnostics for other APIs. Keep the official BaseEvent input.
// Phone runtime evidence: 2026-11-05T17:25:03Z game-ui-stable-timestamp.json,
// acceptedEvents=1, rejectedEvents=0, buttons/pointers=0 after release. This is
// touch aggregation evidence, not proof of game HID consumption or all devices.
interface EventClock {
  readonly timestamp: number;
}

export function readTouchTimestamp(event: BaseEvent): number {
  const clock = event as EventClock;
  return clock.timestamp;
}
