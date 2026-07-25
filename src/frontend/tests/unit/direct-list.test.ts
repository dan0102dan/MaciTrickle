import { strictEqual } from "node:assert";
import { describe, it } from "jsr:@std/testing@1.0.19/bdd";

import { installSvelteRunesMocks } from "../mocks/setup-svelte-runes";

/* src/types pulls in the interface store transitively, so the rune mocks
   have to be in place before it is evaluated -- hence the dynamic imports
   below, same as groups-store-mutations.test.ts. */
installSvelteRunesMocks();

const { DIRECT_INTERFACE, GroupSchema, isDirectList } = await import("../../src/types");
const { parse } = await import("valibot");

const baseGroup = {
  id: "0123abcd",
  name: "bypass",
  interface: "wg0",
  enable: true,
  rules: [],
};

describe("Direct lists", () => {
  it("recognises a group pointed at the reserved interface", () => {
    const routing = parse(GroupSchema, baseGroup);
    const direct = parse(GroupSchema, { ...baseGroup, interface: DIRECT_INTERFACE });

    strictEqual(isDirectList(routing), false);
    strictEqual(isDirectList(direct), true);
  });

  it("recognises a subscription the same way", () => {
    // Groups and subscriptions both carry an interface, which is what lets a
    // downloaded rule-set be a bypass list without any extra field.
    strictEqual(isDirectList({ interface: DIRECT_INTERFACE }), true);
    strictEqual(isDirectList({ interface: "blackhole" }), false);
  });

  it("keeps a group's schema free of routing-mode fields", () => {
    // The backend has no mode/onException/routeLocal any more; a config that
    // still carries them must parse without them leaking into the group.
    const group = parse(GroupSchema, {
      ...baseGroup,
      mode: "except",
      onException: "mainroute",
      routeLocal: true,
    });
    strictEqual("mode" in group, false);
    strictEqual("onException" in group, false);
    strictEqual("routeLocal" in group, false);
  });
});
