/* Caller receives binding METADATA only; no point/value/epoch is resurrected.
 * Real native ctor/dtor/source rebind retirement clears active and fails this.
 * Ordinary DATA invalidation preserves active binding with UNAVAILABLE status. */
