import { deepStrictEqual, strictEqual } from "node:assert";
import { describe, it } from "jsr:@std/testing@1.0.19/bdd";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

/* src/types pulls in the interface store transitively, so the rune mocks
   have to be in place before it is evaluated -- hence the dynamic imports
   below, same as groups-store-mutations.test.ts. */
installSvelteRunesMocks();

const { GroupSchema, PORT_RULE_TYPE, RULE_TYPES, isExceptGroup, ruleTypesForGroup } =
  await import("../../src/types");
const { parse } = await import("valibot");

const baseGroup = {
  id: "0123abcd",
  name: "vpn",
  interface: "wg0",
  enable: true,
  rules: [],
};

describe("Group routing mode", () => {
  it("treats a group without mode keys as a normal group", () => {
    // The backend omits the mode keys entirely for ordinary groups, so an
    // absent key must not be readable as anything but "normal".
    const group = parse(GroupSchema, baseGroup);
    strictEqual(group.mode, "normal");
    strictEqual(group.onException, "continue");
    strictEqual(group.routeLocal, false);
    strictEqual(isExceptGroup(group), false);
  });

  it("round-trips an except group", () => {
    const group = parse(GroupSchema, {
      ...baseGroup,
      mode: "except",
      onException: "mainroute",
      routeLocal: true,
    });
    strictEqual(isExceptGroup(group), true);
    strictEqual(group.onException, "mainroute");
    strictEqual(group.routeLocal, true);
  });

  it("defaults an except group's excepted traffic to continuing", () => {
    const group = parse(GroupSchema, { ...baseGroup, mode: "except" });
    strictEqual(isExceptGroup(group), true);
    strictEqual(group.onException, "continue");
    // Local networks are left alone unless asked for: an except group marks
    // everything, and routing the LAN into the tunnel breaks it.
    strictEqual(group.routeLocal, false);
  });

  it("offers the port rule type only inside an except group", () => {
    const normal = parse(GroupSchema, baseGroup);
    const except = parse(GroupSchema, { ...baseGroup, mode: "except" });

    deepStrictEqual(ruleTypesForGroup(normal), RULE_TYPES);
    strictEqual(
      ruleTypesForGroup(normal).some((t) => t.value === "port"),
      false,
    );
    strictEqual(
      ruleTypesForGroup(except).some((t) => t.value === PORT_RULE_TYPE.value),
      true,
    );
  });
});
