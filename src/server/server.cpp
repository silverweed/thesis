#include "server.hpp"

/* Memory is used like this:
 * [66%] resources
 * [10%] scene
 * [24%] active EP tmp buffer
 */
Server::Server(std::size_t memsize)
	: memory(memsize)
	, activeEP{ *this }
	, passiveEP{ *this }
	, relEP{ *this }
{
	resources.init(memory.data(), memsize * 2 / 3);
	scene.init(memory.data() + resources.getMemsize(), memsize / 10);
	activeEP.init(memory.data() + resources.getMemsize() + scene.getMemsize(),
		memsize - resources.getMemsize() - scene.getMemsize());

	assert(resources.getMemsize() + scene.getMemsize() + activeEP.getMemsize() <= memsize);
}

Server::~Server()
{
	closeNetwork();
}

void Server::closeNetwork()
{
	activeEP.close();
	passiveEP.close();
	relEP.close();
}
