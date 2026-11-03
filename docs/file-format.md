A record contains 7 bytes header which contains crc32c sum,payload-size and type and the rest block is the payload.

the crc32c is 4 bytes long used to check if the data is written right. and there is 2 bytes to store the length of the payload and lastly there is type which is 5 types which are Zero,Full,First,Middle,Last.

The block rules are if a size of the payload is more than the blocksize - blockheader then that means it's the user fault so we return a invalid arguement. there is also we will have trailing zeros if the size of record is lower than 7bytes.
