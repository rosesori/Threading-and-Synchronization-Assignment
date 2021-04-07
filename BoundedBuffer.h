#ifndef BoundedBuffer_h
#define BoundedBuffer_h

#include <stdio.h>
#include <queue>
#include <string>
#include <pthread.h>
#include <condition_variable>

using namespace std;

class BoundedBuffer
{
private:
	int cap; // max number of bytes in the buffer
	queue<vector<char>> q;	/* the queue of items in the buffer. Note
	that each item a sequence of characters that is best represented by a vector<char> for 2 reasons:
	1. An STL std::string cannot keep binary/non-printables
	2. The other alternative is keeping a char* for the sequence and an integer length (i.e., the items can be of variable length).
	While this would work, it is clearly more tedious */

	// Add necessary synchronization variables and data structures 
	mutex mtx;
	int occupancy;
	/*cond. that tells the consumers that some data is there */
	condition_variable data_avail;
	/*cond. that tells the producers some slot is available */
	condition_variable slot_avail;

public:
	BoundedBuffer(int _cap):cap(_cap), occupancy(0){

	}
	
	~BoundedBuffer(){

	}

	void push(char* data, int len){
		//1. Wait until there is room in the queue (i.e., queue lengh is less than cap)
		//2. Convert the incoming byte sequence given by data and len into a vector<char>
		//3. Then push the vector at the end of the queue

		unique_lock<mutex> l (mtx);
		// keep waiting as long as q.size() == 0
		slot_avail.wait (l, [this]{return occupancy+sizeof(datamsg) < cap;});
		// push onto the queue
		vector<char> d (data, data+len);
		q.push(d);
		occupancy += len;
		// notify consumer that some data is available
		data_avail.notify_one();
		// unlock the mutex so that others can go in
		l.unlock();
	}

	int pop(char* buf, int bufcap){

		unique_lock<mutex> l (mtx);

		//1. Wait until the queue has at least 1 itemKeep waiting as long as q.size() == 0
		data_avail.wait (l, [this]{return q.size() > 0;});

		//2. Pop the front item of the queue. The popped item is a vector<char>
		vector<char> data = q.front();
		q.pop();
		occupancy -= data.size();

		//3. Convert the popped vector<char> into a char*, copy that into buf, make sure that vector<char>'s length is <= bufcap
		//assert (d.size() <= bufcap);
		memcpy (buf, data.data(), data.size());

		// notify any potential producer(s)
		slot_avail.notify_one();
		
		// unlock the mutex so that others can go in
		l.unlock();

		//4. Return the vector's length to the caller so that he knows many bytes were popped
		return data.size();

		//3. Convert the popped vector<char> into a char*, copy that into buf, make sure that vector<char>'s length is <= bufcap
		
	}
};

#endif /* BoundedBuffer_ */
