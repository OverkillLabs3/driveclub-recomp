#include "../gpu/shadps4/video_core/renderer_vulkan/motion_history.h"
#include <cassert>
#include <cstdio>

int main() {
  using namespace Vulkan::Motion;
  History history(128);
  Draw batch{.shader = 11, .geometry = 99, .indices = 21, .topology = 31,
             .index_count = 9, .instances = 2, .first_instance = 4,
             .vertices = {7, 3}};
  history.NextFrame();
  const auto a = history.PrepareInstance(batch, 101);
  const auto b = history.PrepareInstance(batch, 102);
  assert(a.store && b.store && !a.load && !b.load);
  assert(a.instances == 1 && a.first_instance == 0);

  history.NextFrame();
  batch.geometry = 100; // The enclosing batch identity changed.
  batch.instances = 3;
  batch.first_instance = 17;
  const auto reordered_b = history.PrepareInstance(batch, 102);
  const auto inserted_c = history.PrepareInstance(batch, 103);
  const auto reordered_a = history.PrepareInstance(batch, 101);
  assert(reordered_b.load == b.store);
  assert(reordered_a.load == a.store);
  assert(inserted_c.store && !inserted_c.load);

  history.NextFrame();
  batch.instances = 1;
  batch.first_instance = 0;
  const auto retained_a = history.PrepareInstance(batch, 101);
  assert(retained_a.load == reordered_a.store);
  batch.topology++;
  assert(!history.PrepareInstance(batch, 102).load);
  batch.topology--;
  batch.vertices.first++;
  assert(!history.PrepareInstance(batch, 102).load);
  batch.vertices.first--;
  batch.shader++;
  assert(!history.PrepareInstance(batch, 102).load);

  history.NextFrame();
  batch.shader--;
  assert(!history.PrepareInstance(batch, 103).load); // Not present last frame.

  History small(3);
  small.NextFrame();
  assert(small.PrepareInstance(batch, 101).store);
  assert(!small.PrepareInstance(batch, 102).store);
  small.NextFrame();
  assert(small.PrepareInstance(batch, 101).load);
  assert(!small.PrepareInstance(batch, 102).load);
  std::puts("PASS: instance reorder, insertion, removal, base instance, topology, range, shader and capacity isolation");
}
