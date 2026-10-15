On GH100 and GB100 chips the memory subsystem is divided into two primary partitions each servicing 4 GPCs. When a GPC on one partition reads from memory attached to the other partition the request is sent to a local L2 cache slice (local caching node) and then direct over the LTCFABRIC to the remote memory partition L2 cache slice (point of coherence).

注意一下模板的问题。