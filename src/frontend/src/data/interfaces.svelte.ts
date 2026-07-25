import { type Interfaces } from "../types";
import { fetcher } from "../utils/fetcher";

export type InterfaceOption = Interfaces["interfaces"][number];

export const interfaces = $state({
  list: [] as InterfaceOption[],
});

/* Two entries the backend adds that are not devices. Naming them here
   keeps groups and subscriptions describing them the same way, since both
   render this list. */
export const RESERVED_INTERFACE_DESCRIPTIONS: Record<string, string> = {
  blackhole: "Drop this traffic",
  direct: "Leave alone — bypasses tunnels",
};

export function describeInterface(item: InterfaceOption): string | undefined {
  return item.name || RESERVED_INTERFACE_DESCRIPTIONS[item.id] || undefined;
}

export async function fetchInterfaces() {
  try {
    const data = await fetcher.get<Interfaces>("/system/interfaces");
    interfaces.list = data.interfaces;
  } catch (error) {
    console.error("Failed to fetch interfaces:", error);
    interfaces.list = [];
  }
}
