A record contains 7 bytes header which contains crc32c sum,payload-size and type and the rest block is the payload.

the crc32c is 4 bytes long used to check if the data is written right. and there is 2 bytes to store the length of the payload and lastly there is type which is 5 types which are Zero,Full,First,Middle,Last.

The block rules are if a size of the payload is more than the blocksize - blockheader then that means it's the user fault so we return a invalid arguement. there is also we will have trailing zeros if the size of record is lower than 7bytes.

it's saves more space to do prefix compression on the sorted keys rather than random keys because of the what entry stores which is shared,non shared,value_length,key-delta,value and the prefix compression is done on the key_delta and the more there is shared prefix the better the compression leading to better space handling for example take:

User: 1000, User 1001, User 1002 all start with user: 100 which is shared of 8 and unshared of 1 while random keys has almost nothing in shared.

but there is a problem of knowing whrere an entry belongs like if we are tasked to look for entry 500 how do we know where it is? we will have to decode everything till 499 to get it. to solve this we have a restart point it stores key in full and its offset in a restart array. this is useful by helping us do binary search to find it sooner but this doesn't let us know the entry it just makes it faster

so we just find the interval it is in and we will do a linear search which is also faster than binary search in this case because the cache locality (spatial locality of the cache) since it loads the next entry before we call we finish decoding this one.

so seek costs: log2(restarts)+ interval/2 entry decodes instead of n/2.
