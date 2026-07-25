import {
  array,
  boolean,
  fallback,
  length,
  number,
  object,
  optional,
  parse,
  pipe,
  regex,
  string,
  type InferOutput,
} from "valibot";

import { randomId } from "./utils/defaults";

declare global {
  interface WindowEventMap {
    overlay: CustomEvent<{
      content: string;
      type: "show" | "hide";
    }>;

    toast: CustomEvent<{
      content: string;
      type: "info" | "success" | "error" | "warning";
    }>;
  }
}

export function parseConfig(json: string): Config {
  return parse(ConfigSchema, JSON.parse(json));
}

export const RuleSchema = object({
  enable: fallback(boolean(), true),
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  rule: string(),
  type: fallback(string(), "namespace"),
});
export type Rule = InferOutput<typeof RuleSchema>;

export const GroupSchema = object({
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  color: fallback(optional(string()), "#ffffff"),
  interface: string(),
  enable: fallback(boolean(), true),
  /* Routing mode. The backend omits these keys for ordinary groups, so
     absent must mean "normal" — see GROUP_MODES below. */
  mode: fallback(optional(string()), "normal"),
  onException: fallback(optional(string()), "continue"),
  routeLocal: fallback(optional(boolean()), false),
  rules: array(RuleSchema),
});
export type Group = InferOutput<typeof GroupSchema>;

export const SubscriptionRuleSchema = object({
  enable: fallback(boolean(), true),
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  rule: string(),
  type: fallback(string(), "namespace"),
});
export type SubscriptionRule = InferOutput<typeof SubscriptionRuleSchema>;

export const SubscriptionSchema = object({
  id: fallback(pipe(string(), length(8), regex(/^[0-9a-f]{8}/)), randomId()),
  name: fallback(string(), ""),
  interface: string(),
  enable: fallback(boolean(), true),
  rules: array(SubscriptionRuleSchema),
  url: string(),
  lastUpdate: fallback(optional(number()), 0),
  interval: fallback(optional(number()), 86400),
});
export type Subscription = InferOutput<typeof SubscriptionSchema>;

export const ConfigSchema = object({
  groups: array(GroupSchema),
});
export type Config = InferOutput<typeof ConfigSchema>;

export const RULE_TYPES = [
  { value: "namespace", label: "Namespace" },
  { value: "wildcard", label: "Wildcard" },
  { value: "regex", label: "Regex" },
  { value: "domain", label: "Domain" },
  { value: "subnet", label: "IPv4 subnet" },
  { value: "subnet6", label: "IPv6 subnet" },
];

/* `port` matches on the transport header, which the packet path can only do
   inside an "everything except" group's chain — an ordinary group routes by
   address-set membership and has nowhere to put it. So it is offered only
   where it does something. */
export const PORT_RULE_TYPE = { value: "port", label: "Port" };

export function ruleTypesForGroup(group: Pick<Group, "mode">) {
  return isExceptGroup(group) ? [...RULE_TYPES, PORT_RULE_TYPE] : RULE_TYPES;
}

/* A group either selects what to route (normal) or what to leave alone
   (except). Spelled out as a mode rather than an "invert" flag: reading
   "route everything through wg0 except these" is unambiguous, while a
   negated matcher is read wrong about as often as it is read right. */
export const GROUP_MODES = [
  { value: "normal", label: "Normal group" },
  { value: "except", label: "Everything except the conditions below" },
];

export const ON_EXCEPTION_MODES = [
  { value: "continue", label: "Keep checking the other groups" },
  { value: "mainroute", label: "Use the main route" },
];

export function isExceptGroup(group: Pick<Group, "mode">): boolean {
  return group.mode === "except";
}

export type Interfaces = {
  interfaces: {
    id: string;
    name?: string;
  }[];
};
