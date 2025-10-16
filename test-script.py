from m5.objects import SectorTags

from gem5.components.boards.simple_board import SimpleBoard
from gem5.components.cachehierarchies.classic.private_l1_cache_hierarchy import (
    PrivateL1CacheHierarchy,
)
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# This check ensures the gem5 binary contains the ARM ISA target. If not, an
# exception will be thrown.
requires(isa_required=ISA.ARM)

tags = SectorTags(num_blocks_per_sector=4)

# In this setup we don't have a cache. `NoCache` can be used for such setups.
cache_hierarchy = PrivateL1CacheHierarchy(
    l1d_size="256MiB", l1i_size="256MiB", assoc=2, tags=tags
)

# We use a single channel DDR3_1600 memory system
memory = SingleChannelDDR3_1600(size="32MiB")

# We use a simple Timing processor with one core.
processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.ARM, num_cores=1)

# The gem5 library simple board which can be used to run SE-mode simulations.
board = SimpleBoard(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

board.cache_line_size = 32  # 32 bytes

# Here we set the workload. In this case we want to run a simple "Hello World!"
# program compiled to the ARM ISA. The `obtain_resource` function will
# automatically download the binary from the gem5 Resources cloud bucket if
# it's not already present.
board.set_se_binary_workload(
    obtain_resource("arm-hello64-static", resource_version="1.0.0")
)

# Lastly we run the simulation.
simulator = Simulator(board=board)
simulator.run()
