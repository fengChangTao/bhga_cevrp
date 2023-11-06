#include "LocalSearch.h" 

void LocalSearch::run(Individual & indiv, double penaltyCapacityLS, double penaltyDurationLS)
{
	this->penaltyCapacityLS = penaltyCapacityLS;
	this->penaltyDurationLS = penaltyDurationLS;
	loadIndividual(indiv);

	// 打乱LS探索的节点的顺序，以便在搜索中有更多的多样性
	std::shuffle(orderNodes.begin(), orderNodes.end(), params.ran);
	std::shuffle(orderRoutes.begin(), orderRoutes.end(), params.ran);
	for (int i = 1; i <= params.nbClients; i++)
		if (params.ran() % params.ap.nbGranular == 0)  // 平均0 (n/nbGranular)次调用内部函数，以实现整体的线性时间复杂度
			std::shuffle(params.correlatedVertices[i].begin(), params.correlatedVertices[i].end(), params.ran);

	searchCompleted = false;
	for (loopID = 0; !searchCompleted; loopID++)
	{
		if (loopID > 1) // 允许至少两个循环，因为一些涉及空路线的移动在第一个循环时没有被检查
			searchCompleted = true;

		/* 经典路线改进(RI)的移动受到邻近限制 */
		for (int posU = 0; posU < params.nbClients; posU++)
		{
			nodeU = &clients[orderNodes[posU]];
			int lastTestRINodeU = nodeU->whenLastTestedRI;
			nodeU->whenLastTestedRI = nbMoves;
			for (int posV = 0; posV < (int)params.correlatedVertices[nodeU->cour].size(); posV++)
			{
				nodeV = &clients[params.correlatedVertices[nodeU->cour][posV]];
				if (loopID == 0 || std::max<int>(nodeU->route->whenLastModified, nodeV->route->whenLastModified) > lastTestRINodeU) // 仅评估自上次移动评估以来已修改的路线中涉及的移动，用于节点nodeU
				{
					// 在这个循环中随机化邻域的顺序并不十分重要，因为我们已经随机化了节点对的顺序（对于同一节点对，找到不同类型的改进移动并不常见）。
					setLocalVariablesRouteU();
					setLocalVariablesRouteV();
					if (move1()) continue; // RELOCATE
					if (move2()) continue; // RELOCATE
					if (move3()) continue; // RELOCATE
					if (nodeUIndex <= nodeVIndex && move4()) continue; // SWAP
					if (move5()) continue; // SWAP
					if (nodeUIndex <= nodeVIndex && move6()) continue; // SWAP
					if (intraRouteMove && move7()) continue; // 2-OPT
					if (!intraRouteMove && move8()) continue; // 2-OPT*
					if (!intraRouteMove && move9()) continue; // 2-OPT*

					// 尝试将节点nodeU直接插入到仓库之后的移动
					if (nodeV->prev->isDepot)
					{
						nodeV = nodeV->prev;
						setLocalVariablesRouteV();
						if (move1()) continue; // RELOCATE
						if (move2()) continue; // RELOCATE
						if (move3()) continue; // RELOCATE
						if (!intraRouteMove && move8()) continue; // 2-OPT*
						if (!intraRouteMove && move9()) continue; // 2-OPT*
					}
				}
			}

			/* 涉及空路径的移动，在第一个循环中未经测试，以避免过度增加车队规模。 */
			if (loopID > 0 && !emptyRoutes.empty())
			{
				nodeV = routes[*emptyRoutes.begin()].depot;
				setLocalVariablesRouteU();
				setLocalVariablesRouteV();
				if (move1()) continue; // RELOCATE
				if (move2()) continue; // RELOCATE
				if (move3()) continue; // RELOCATE
				if (move9()) continue; // 2-OPT*
			}
		}

		if (params.ap.useSwapStar == 1 && params.areCoordinatesProvided)
		{
			/* （SWAP*）移动限制在圆形扇区重叠的路径对之间 */
			for (int rU = 0; rU < params.nbVehicles; rU++)
			{
				routeU = &routes[orderRoutes[rU]];
				int lastTestSWAPStarRouteU = routeU->whenLastTestedSWAPStar;
				routeU->whenLastTestedSWAPStar = nbMoves;
				for (int rV = 0; rV < params.nbVehicles; rV++)
				{
					routeV = &routes[orderRoutes[rV]];
					if (routeU->nbCustomers > 0 && routeV->nbCustomers > 0 && routeU->cour < routeV->cour
						&& (loopID == 0 || std::max<int>(routeU->whenLastModified, routeV->whenLastModified)
							> lastTestSWAPStarRouteU))
						if (CircleSector::overlap(routeU->sector, routeV->sector))
							swapStar();
				}
			}
		}
	}

	// 将局部搜索产生的解决方案注册到个体中。
	exportIndividual(indiv);
}

void LocalSearch::setLocalVariablesRouteU()
{
	routeU = nodeU->route;
	nodeX = nodeU->next;
	nodeXNextIndex = nodeX->next->cour;
	nodeUIndex = nodeU->cour;
	nodeUPrevIndex = nodeU->prev->cour;
	nodeXIndex = nodeX->cour;
	loadU    = params.cli[nodeUIndex].demand;
	serviceU = params.cli[nodeUIndex].serviceDuration;
	loadX	 = params.cli[nodeXIndex].demand;
	serviceX = params.cli[nodeXIndex].serviceDuration;
}

void LocalSearch::setLocalVariablesRouteV()
{
	routeV = nodeV->route;
	nodeY = nodeV->next;
	nodeYNextIndex = nodeY->next->cour;
	nodeVIndex = nodeV->cour;
	nodeVPrevIndex = nodeV->prev->cour;
	nodeYIndex = nodeY->cour;
	loadV    = params.cli[nodeVIndex].demand;
	serviceV = params.cli[nodeVIndex].serviceDuration;
	loadY	 = params.cli[nodeYIndex].demand;
	serviceY = params.cli[nodeYIndex].serviceDuration;
	intraRouteMove = (routeU == routeV);
}

bool LocalSearch::move1()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeXIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeUIndex][nodeXIndex];
	double costSuppV = params.timeCost[nodeVIndex][nodeUIndex] + params.timeCost[nodeUIndex][nodeYIndex] - params.timeCost[nodeVIndex][nodeYIndex];
    // 路径剪枝
	if (!intraRouteMove) // 如果不在同一路径
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU - serviceU)
			+ penaltyExcessLoad(routeU->load - loadU)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV + serviceU)
			+ penaltyExcessLoad(routeV->load + loadU)
			- routeV->penalty;
	}

//    if(dis3(params.ran)>gai)
//        goto F1;
	if (costSuppU + costSuppV > -MY_EPSILON) return false;  // 如果VRP路径变长了
    F1:
	if (nodeUIndex == nodeYIndex) return false; // 如果u本来就在v后面了


    vector<int> R_u=routeU->rrr;
    vector<int> R_v=routeV->rrr;
    if(!intraRouteMove)
    {
        remove_f3(R_u,{nodeUIndex});
        add_f3(R_v,{nodeUIndex},nodeVIndex);
        double dian2=insertStationByRemove2(R_u,params.c_evrp).second;
        double dian3=insertStationByRemove2(R_v,params.c_evrp).second;

        if((dian2+dian3-routeU->fit_charge-routeV->fit_charge)>-MY_EPSILON)
            return false;
    }
    else
    {
        remove_f3(R_u,{nodeUIndex});
        add_f3(R_u,{nodeUIndex},nodeVIndex);
        double dian4=insertStationByRemove2(R_u,params.c_evrp).second;
        if((dian4-routeU->fit_charge)>-MY_EPSILON)
            return false;
    }

    // 这里是已经决定更改了
	insertNode(nodeU, nodeV);   // 更改"受影响节点"的前后指针
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move2()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeXNextIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeXIndex][nodeXNextIndex];
	double costSuppV = params.timeCost[nodeVIndex][nodeUIndex] + params.timeCost[nodeXIndex][nodeYIndex] - params.timeCost[nodeVIndex][nodeYIndex];

	if (!intraRouteMove)
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU - params.timeCost[nodeUIndex][nodeXIndex] - serviceU - serviceX)
			+ penaltyExcessLoad(routeU->load - loadU - loadX)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV + params.timeCost[nodeUIndex][nodeXIndex] + serviceU + serviceX)
			+ penaltyExcessLoad(routeV->load + loadU + loadX)
			- routeV->penalty;
	}

	if (costSuppU + costSuppV > -MY_EPSILON) return false;
	if (nodeU == nodeY || nodeV == nodeX || nodeX->isDepot) return false;

	insertNode(nodeU, nodeV);
	insertNode(nodeX, nodeU);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move3()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeXNextIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeUIndex][nodeXIndex] - params.timeCost[nodeXIndex][nodeXNextIndex];
	double costSuppV = params.timeCost[nodeVIndex][nodeXIndex] + params.timeCost[nodeXIndex][nodeUIndex] + params.timeCost[nodeUIndex][nodeYIndex] - params.timeCost[nodeVIndex][nodeYIndex];

	if (!intraRouteMove)
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU - serviceU - serviceX)
			+ penaltyExcessLoad(routeU->load - loadU - loadX)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV + serviceU + serviceX)
			+ penaltyExcessLoad(routeV->load + loadU + loadX)
			- routeV->penalty;
	}

	if (costSuppU + costSuppV > -MY_EPSILON) return false;
	if (nodeU == nodeY || nodeX == nodeV || nodeX->isDepot) return false;

	insertNode(nodeX, nodeV);
	insertNode(nodeU, nodeX);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move4()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeVIndex] + params.timeCost[nodeVIndex][nodeXIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeUIndex][nodeXIndex];
	double costSuppV = params.timeCost[nodeVPrevIndex][nodeUIndex] + params.timeCost[nodeUIndex][nodeYIndex] - params.timeCost[nodeVPrevIndex][nodeVIndex] - params.timeCost[nodeVIndex][nodeYIndex];

	if (!intraRouteMove)
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU + serviceV - serviceU)
			+ penaltyExcessLoad(routeU->load + loadV - loadU)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV - serviceV + serviceU)
			+ penaltyExcessLoad(routeV->load + loadU - loadV)
			- routeV->penalty;
	}

	if (costSuppU + costSuppV > -MY_EPSILON) return false;
	if (nodeUIndex == nodeVPrevIndex || nodeUIndex == nodeYIndex) return false;

	swapNode(nodeU, nodeV);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move5()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeVIndex] + params.timeCost[nodeVIndex][nodeXNextIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeXIndex][nodeXNextIndex];
	double costSuppV = params.timeCost[nodeVPrevIndex][nodeUIndex] + params.timeCost[nodeXIndex][nodeYIndex] - params.timeCost[nodeVPrevIndex][nodeVIndex] - params.timeCost[nodeVIndex][nodeYIndex];

	if (!intraRouteMove)
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU - params.timeCost[nodeUIndex][nodeXIndex] + serviceV - serviceU - serviceX)
			+ penaltyExcessLoad(routeU->load + loadV - loadU - loadX)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV + params.timeCost[nodeUIndex][nodeXIndex] - serviceV + serviceU + serviceX)
			+ penaltyExcessLoad(routeV->load + loadU + loadX - loadV)
			- routeV->penalty;
	}

	if (costSuppU + costSuppV > -MY_EPSILON) return false;
	if (nodeU == nodeV->prev || nodeX == nodeV->prev || nodeU == nodeY || nodeX->isDepot) return false;

	swapNode(nodeU, nodeV);
	insertNode(nodeX, nodeU);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move6()
{
	double costSuppU = params.timeCost[nodeUPrevIndex][nodeVIndex] + params.timeCost[nodeYIndex][nodeXNextIndex] - params.timeCost[nodeUPrevIndex][nodeUIndex] - params.timeCost[nodeXIndex][nodeXNextIndex];
	double costSuppV = params.timeCost[nodeVPrevIndex][nodeUIndex] + params.timeCost[nodeXIndex][nodeYNextIndex] - params.timeCost[nodeVPrevIndex][nodeVIndex] - params.timeCost[nodeYIndex][nodeYNextIndex];

	if (!intraRouteMove)
	{
		// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
		if (costSuppU + costSuppV >= routeU->penalty + routeV->penalty) return false;

		costSuppU += penaltyExcessDuration(routeU->duration + costSuppU - params.timeCost[nodeUIndex][nodeXIndex] + params.timeCost[nodeVIndex][nodeYIndex] + serviceV + serviceY - serviceU - serviceX)
			+ penaltyExcessLoad(routeU->load + loadV + loadY - loadU - loadX)
			- routeU->penalty;

		costSuppV += penaltyExcessDuration(routeV->duration + costSuppV + params.timeCost[nodeUIndex][nodeXIndex] - params.timeCost[nodeVIndex][nodeYIndex] - serviceV - serviceY + serviceU + serviceX)
			+ penaltyExcessLoad(routeV->load + loadU + loadX - loadV - loadY)
			- routeV->penalty;
	}

	if (costSuppU + costSuppV > -MY_EPSILON) return false;
	if (nodeX->isDepot || nodeY->isDepot || nodeY == nodeU->prev || nodeU == nodeY || nodeX == nodeV || nodeV == nodeX->next) return false;

	swapNode(nodeU, nodeV);
	swapNode(nodeX, nodeY);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	if (!intraRouteMove) updateRouteData(routeV);
	return true;
}

bool LocalSearch::move7()
{
	if (nodeU->position > nodeV->position) return false;

	double cost = params.timeCost[nodeUIndex][nodeVIndex] + params.timeCost[nodeXIndex][nodeYIndex] - params.timeCost[nodeUIndex][nodeXIndex] - params.timeCost[nodeVIndex][nodeYIndex] + nodeV->cumulatedReversalDistance - nodeX->cumulatedReversalDistance;

	if (cost > -MY_EPSILON&&params.isChu==true) return false;

	if (nodeU->next == nodeV) return false;

    if(params.isChu==false)
    {
        auto R_u=routeU->rrr;
        int u=nodeUIndex,v=nodeVIndex;

        auto it_u = find(R_u.begin(), R_u.end(), u);
        auto it_v = find(R_u.begin(), R_u.end(), v);
        if (it_u<it_v)
            std::reverse(it_u, it_v + 1);
        else
            return false;
        double dian2=insertStationByRemove2(R_u,params.c_evrp).second;
        if(dian2-(routeU->fit_charge)>-MY_EPSILON)
            return false;
    }
	Node * nodeNum = nodeX->next;
	nodeX->prev = nodeNum;
	nodeX->next = nodeY;

	while (nodeNum != nodeV)
	{
		Node * temp = nodeNum->next;
		nodeNum->next = nodeNum->prev;
		nodeNum->prev = temp;
		nodeNum = temp;
	}

	nodeV->next = nodeV->prev;
	nodeV->prev = nodeU;
	nodeU->next = nodeV;
	nodeY->prev = nodeX;

	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	return true;
}

bool LocalSearch::move8()
{
	double cost = params.timeCost[nodeUIndex][nodeVIndex] + params.timeCost[nodeXIndex][nodeYIndex] - params.timeCost[nodeUIndex][nodeXIndex] - params.timeCost[nodeVIndex][nodeYIndex]
		+ nodeV->cumulatedReversalDistance + routeU->reversalDistance - nodeX->cumulatedReversalDistance
		- routeU->penalty - routeV->penalty;

	// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
	if (cost >= 0) return false;
		
	cost += penaltyExcessDuration(nodeU->cumulatedTime + nodeV->cumulatedTime + nodeV->cumulatedReversalDistance + params.timeCost[nodeUIndex][nodeVIndex])
		+ penaltyExcessDuration(routeU->duration - nodeU->cumulatedTime - params.timeCost[nodeUIndex][nodeXIndex] + routeU->reversalDistance - nodeX->cumulatedReversalDistance + routeV->duration - nodeV->cumulatedTime - params.timeCost[nodeVIndex][nodeYIndex] + params.timeCost[nodeXIndex][nodeYIndex])
		+ penaltyExcessLoad(nodeU->cumulatedLoad + nodeV->cumulatedLoad)
		+ penaltyExcessLoad(routeU->load + routeV->load - nodeU->cumulatedLoad - nodeV->cumulatedLoad);
		
	if (cost > -MY_EPSILON) return false;

	Node * depotU = routeU->depot;
	Node * depotV = routeV->depot;
	Node * depotUFin = routeU->depot->prev;
	Node * depotVFin = routeV->depot->prev;
	Node * depotVSuiv = depotV->next;

	Node * temp;
	Node * xx = nodeX;
	Node * vv = nodeV;

	while (!xx->isDepot)
	{
		temp = xx->next;
		xx->next = xx->prev;
		xx->prev = temp;
		xx->route = routeV;
		xx = temp;
	}

	while (!vv->isDepot)
	{
		temp = vv->prev;
		vv->prev = vv->next;
		vv->next = temp;
		vv->route = routeU;
		vv = temp;
	}

	nodeU->next = nodeV;
	nodeV->prev = nodeU;
	nodeX->next = nodeY;
	nodeY->prev = nodeX;

	if (nodeX->isDepot)
	{
		depotUFin->next = depotU;
		depotUFin->prev = depotVSuiv;
		depotUFin->prev->next = depotUFin;
		depotV->next = nodeY;
		nodeY->prev = depotV;
	}
	else if (nodeV->isDepot)
	{
		depotV->next = depotUFin->prev;
		depotV->next->prev = depotV;
		depotV->prev = depotVFin;
		depotUFin->prev = nodeU;
		nodeU->next = depotUFin;
	}
	else
	{
		depotV->next = depotUFin->prev;
		depotV->next->prev = depotV;
		depotUFin->prev = depotVSuiv;
		depotUFin->prev->next = depotUFin;
	}

	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	updateRouteData(routeV);
	return true;
}

bool LocalSearch::move9()
{
	double cost = params.timeCost[nodeUIndex][nodeYIndex] + params.timeCost[nodeVIndex][nodeXIndex] - params.timeCost[nodeUIndex][nodeXIndex] - params.timeCost[nodeVIndex][nodeYIndex]
		        - routeU->penalty - routeV->penalty;

	// Early move pruning to save CPU time. Guarantees that this move cannot improve without checking additional (load, duration...) constraints
	if (cost >= 0) return false;
		
	cost += penaltyExcessDuration(nodeU->cumulatedTime + routeV->duration - nodeV->cumulatedTime - params.timeCost[nodeVIndex][nodeYIndex] + params.timeCost[nodeUIndex][nodeYIndex])
		+ penaltyExcessDuration(routeU->duration - nodeU->cumulatedTime - params.timeCost[nodeUIndex][nodeXIndex] + nodeV->cumulatedTime + params.timeCost[nodeVIndex][nodeXIndex])
		+ penaltyExcessLoad(nodeU->cumulatedLoad + routeV->load - nodeV->cumulatedLoad)
		+ penaltyExcessLoad(nodeV->cumulatedLoad + routeU->load - nodeU->cumulatedLoad);

	if (cost > -MY_EPSILON) return false;

	Node * depotU = routeU->depot;
	Node * depotV = routeV->depot;
	Node * depotUFin = depotU->prev;
	Node * depotVFin = depotV->prev;
	Node * depotUpred = depotUFin->prev;

	Node * count = nodeY;
	while (!count->isDepot)
	{
		count->route = routeU;
		count = count->next;
	}

	count = nodeX;
	while (!count->isDepot)
	{
		count->route = routeV;
		count = count->next;
	}

	nodeU->next = nodeY;
	nodeY->prev = nodeU;
	nodeV->next = nodeX;
	nodeX->prev = nodeV;

	if (nodeX->isDepot)
	{
		depotUFin->prev = depotVFin->prev;
		depotUFin->prev->next = depotUFin;
		nodeV->next = depotVFin;
		depotVFin->prev = nodeV;
	}
	else
	{
		depotUFin->prev = depotVFin->prev;
		depotUFin->prev->next = depotUFin;
		depotVFin->prev = depotUpred;
		depotVFin->prev->next = depotVFin;
	}

	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	updateRouteData(routeV);
	return true;
}
// 计算路线U和路线V之间的所有SWAP*，并应用最佳改进移动
bool LocalSearch::swapStar()
{
	SwapStarElement myBestSwapStar;

	// Preprocessing insertion costs
	preprocessInsertions(routeU, routeV);
	preprocessInsertions(routeV, routeU);

	// Evaluating the moves
	for (nodeU = routeU->depot->next; !nodeU->isDepot; nodeU = nodeU->next)
	{
		for (nodeV = routeV->depot->next; !nodeV->isDepot; nodeV = nodeV->next)
		{
			double deltaPenRouteU = penaltyExcessLoad(routeU->load + params.cli[nodeV->cour].demand - params.cli[nodeU->cour].demand) - routeU->penalty;
			double deltaPenRouteV = penaltyExcessLoad(routeV->load + params.cli[nodeU->cour].demand - params.cli[nodeV->cour].demand) - routeV->penalty;

			// Quick filter: possibly early elimination of many SWAP* due to the capacity constraints/penalties and bounds on insertion costs
			if (deltaPenRouteU + nodeU->deltaRemoval + deltaPenRouteV + nodeV->deltaRemoval <= 0)
			{
				SwapStarElement mySwapStar;
				mySwapStar.U = nodeU;
				mySwapStar.V = nodeV;

				// Evaluate best reinsertion cost of U in the route of V where V has been removed
				double extraV = getCheapestInsertSimultRemoval(nodeU, nodeV, mySwapStar.bestPositionU);

				// Evaluate best reinsertion cost of V in the route of U where U has been removed
				double extraU = getCheapestInsertSimultRemoval(nodeV, nodeU, mySwapStar.bestPositionV);

				// Evaluating final cost
				mySwapStar.moveCost = deltaPenRouteU + nodeU->deltaRemoval + extraU + deltaPenRouteV + nodeV->deltaRemoval + extraV
					+ penaltyExcessDuration(routeU->duration + nodeU->deltaRemoval + extraU + params.cli[nodeV->cour].serviceDuration - params.cli[nodeU->cour].serviceDuration)
					+ penaltyExcessDuration(routeV->duration + nodeV->deltaRemoval + extraV - params.cli[nodeV->cour].serviceDuration + params.cli[nodeU->cour].serviceDuration);

				if (mySwapStar.moveCost < myBestSwapStar.moveCost)
					myBestSwapStar = mySwapStar;
			}
		}
	}

	// Including RELOCATE from nodeU towards routeV (costs nothing to include in the evaluation at this step since we already have the best insertion location)
	// Moreover, since the granularity criterion is different, this can lead to different improving moves
	for (nodeU = routeU->depot->next; !nodeU->isDepot; nodeU = nodeU->next)
	{
		SwapStarElement mySwapStar;
		mySwapStar.U = nodeU;
		mySwapStar.bestPositionU = bestInsertClient[routeV->cour][nodeU->cour].bestLocation[0];
		double deltaDistRouteU = params.timeCost[nodeU->prev->cour][nodeU->next->cour] - params.timeCost[nodeU->prev->cour][nodeU->cour] - params.timeCost[nodeU->cour][nodeU->next->cour];
		double deltaDistRouteV = bestInsertClient[routeV->cour][nodeU->cour].bestCost[0];
		mySwapStar.moveCost = deltaDistRouteU + deltaDistRouteV
			+ penaltyExcessLoad(routeU->load - params.cli[nodeU->cour].demand) - routeU->penalty
			+ penaltyExcessLoad(routeV->load + params.cli[nodeU->cour].demand) - routeV->penalty
			+ penaltyExcessDuration(routeU->duration + deltaDistRouteU - params.cli[nodeU->cour].serviceDuration)
			+ penaltyExcessDuration(routeV->duration + deltaDistRouteV + params.cli[nodeU->cour].serviceDuration);

		if (mySwapStar.moveCost < myBestSwapStar.moveCost)
			myBestSwapStar = mySwapStar;
	}

	// Including RELOCATE from nodeV towards routeU
	for (nodeV = routeV->depot->next; !nodeV->isDepot; nodeV = nodeV->next)
	{
		SwapStarElement mySwapStar;
		mySwapStar.V = nodeV;
		mySwapStar.bestPositionV = bestInsertClient[routeU->cour][nodeV->cour].bestLocation[0];
		double deltaDistRouteU = bestInsertClient[routeU->cour][nodeV->cour].bestCost[0];
		double deltaDistRouteV = params.timeCost[nodeV->prev->cour][nodeV->next->cour] - params.timeCost[nodeV->prev->cour][nodeV->cour] - params.timeCost[nodeV->cour][nodeV->next->cour];
		mySwapStar.moveCost = deltaDistRouteU + deltaDistRouteV
			+ penaltyExcessLoad(routeU->load + params.cli[nodeV->cour].demand) - routeU->penalty
			+ penaltyExcessLoad(routeV->load - params.cli[nodeV->cour].demand) - routeV->penalty
			+ penaltyExcessDuration(routeU->duration + deltaDistRouteU + params.cli[nodeV->cour].serviceDuration)
			+ penaltyExcessDuration(routeV->duration + deltaDistRouteV - params.cli[nodeV->cour].serviceDuration);

		if (mySwapStar.moveCost < myBestSwapStar.moveCost)
			myBestSwapStar = mySwapStar;
	}

	if (myBestSwapStar.moveCost > -MY_EPSILON) return false;

	// Applying the best move in case of improvement
	if (myBestSwapStar.bestPositionU != NULL) insertNode(myBestSwapStar.U, myBestSwapStar.bestPositionU);
	if (myBestSwapStar.bestPositionV != NULL) insertNode(myBestSwapStar.V, myBestSwapStar.bestPositionV);
	nbMoves++; // Increment move counter before updating route data
	searchCompleted = false;
	updateRouteData(routeU);
	updateRouteData(routeV);
	return true;
}
// 计算V在路线中的插入成本和位置，其中V被省略
double LocalSearch::getCheapestInsertSimultRemoval(Node * U, Node * V, Node *& bestPosition)
{
	ThreeBestInsert * myBestInsert = &bestInsertClient[V->route->cour][U->cour];
	bool found = false;

	// Find best insertion in the route such that V is not next or pred (can only belong to the top three locations)
	bestPosition = myBestInsert->bestLocation[0];
	double bestCost = myBestInsert->bestCost[0];
	found = (bestPosition != V && bestPosition->next != V);
	if (!found && myBestInsert->bestLocation[1] != NULL)
	{
		bestPosition = myBestInsert->bestLocation[1];
		bestCost = myBestInsert->bestCost[1];
		found = (bestPosition != V && bestPosition->next != V);
		if (!found && myBestInsert->bestLocation[2] != NULL)
		{
			bestPosition = myBestInsert->bestLocation[2];
			bestCost = myBestInsert->bestCost[2];
			found = true;
		}
	}

	// Compute insertion in the place of V
	double deltaCost = params.timeCost[V->prev->cour][U->cour] + params.timeCost[U->cour][V->next->cour] - params.timeCost[V->prev->cour][V->next->cour];
	if (!found || deltaCost < bestCost)
	{
		bestPosition = V->prev;
		bestCost = deltaCost;
	}

	return bestCost;
}
// 预处理路线R1中所有节点在路线R2中的插入成本
void LocalSearch::preprocessInsertions(Route * R1, Route * R2)
{
	for (Node * U = R1->depot->next; !U->isDepot; U = U->next)
	{
		// Performs the preprocessing
		U->deltaRemoval = params.timeCost[U->prev->cour][U->next->cour] - params.timeCost[U->prev->cour][U->cour] - params.timeCost[U->cour][U->next->cour];
		if (R2->whenLastModified > bestInsertClient[R2->cour][U->cour].whenLastCalculated)
		{
			bestInsertClient[R2->cour][U->cour].reset();
			bestInsertClient[R2->cour][U->cour].whenLastCalculated = nbMoves;
			bestInsertClient[R2->cour][U->cour].bestCost[0] = params.timeCost[0][U->cour] + params.timeCost[U->cour][R2->depot->next->cour] - params.timeCost[0][R2->depot->next->cour];
			bestInsertClient[R2->cour][U->cour].bestLocation[0] = R2->depot;
			for (Node * V = R2->depot->next; !V->isDepot; V = V->next)
			{
				double deltaCost = params.timeCost[V->cour][U->cour] + params.timeCost[U->cour][V->next->cour] - params.timeCost[V->cour][V->next->cour];
				bestInsertClient[R2->cour][U->cour].compareAndAdd(deltaCost, V);
			}
		}
	}
}
// 解决方案更新：在V之后插入U
void LocalSearch::insertNode(Node * U, Node * V)
{
	U->prev->next = U->next;
	U->next->prev = U->prev;
	V->next->prev = U;
	U->prev = V;
	U->next = V->next;
	V->next = U;
	U->route = V->route;
}
// 解决方案更新：交换U和V
void LocalSearch::swapNode(Node * U, Node * V)
{
	Node * myVPred = V->prev;
	Node * myVSuiv = V->next;
	Node * myUPred = U->prev;
	Node * myUSuiv = U->next;
	Route * myRouteU = U->route;
	Route * myRouteV = V->route;

	myUPred->next = V;
	myUSuiv->prev = V;
	myVPred->next = U;
	myVSuiv->prev = U;

	U->prev = myVPred;
	U->next = myVSuiv;
	V->prev = myUPred;
	V->next = myUSuiv;

	U->route = myRouteV;
	V->route = myRouteU;
}


//在vector中移除一些元素
void LocalSearch::remove_f3(vector<int>& g, vector<int> del)
{
    for (auto h : del)
    {
        auto it2 = find(g.begin(), g.end(), h);
        if (it2 != g.end())
        {
            g.erase(it2);
        }
    }
}
//在vector中的某位置pos后加入一些元素
void LocalSearch::add_f3(vector<int>& g, vector<int> add, int after)
{
    auto it2 = find(g.begin(), g.end(), after);
    if (it2 != g.end())
    {
        g.insert(it2 + 1, add.begin(), add.end());

    }


}

// 更新路线的预处理数据
pair<vector<int>, double> LocalSearch::insertStationByRemove2(vector<int> route, Case& instance)
{
    list<pair<int, int>> stationInserted;
    for (int i = 0; i < (int)route.size() - 1; i++) {
        double allowedDis = instance.maxDis;
        if (i != 0) {
            allowedDis = instance.maxDis - instance.distances[stationInserted.back().second][route[i]];
        }
        int onestation = instance.findNearestStationFeasible(route[i], route[i + 1], allowedDis);
        if (onestation == -1) return make_pair(route, -1);
        stationInserted.push_back(make_pair(i, onestation));
    }
    /*for (int i = 0; i < (int)route.size() - 1; i++) {
        stationInserted.push_back(make_pair(i, instance.bestStation[route[i]][route[i + 1]]));
    }*/
    while (!stationInserted.empty())
    {
        bool changed = false;
        list<pair<int, int>>::iterator delone = stationInserted.begin();
        double savedis = 0;
        list<pair<int, int>>::iterator itr = stationInserted.begin();
        list<pair<int, int>>::iterator next = itr;
        next++;
        if (next != stationInserted.end()) {
            int endInd = next->first;
            int endstation = next->second;
            double sumdis = 0;
            for (int i = 0; i < endInd; i++) {
                sumdis += instance.distances[route[i]][route[i + 1]];
            }
            sumdis += instance.distances[route[endInd]][endstation];
            if (sumdis <= instance.maxDis) {
                savedis = instance.distances[route[itr->first]][itr->second] + instance.distances[itr->second][route[itr->first + 1]]
                          - instance.distances[route[itr->first]][route[itr->first + 1]];
            }
        }
        else {
            double sumdis = 0;
            for (int i = 0; i < (int)route.size() - 1; i++) {
                sumdis += instance.distances[route[i]][route[i + 1]];
            }
            if (sumdis <= instance.maxDis) {
                savedis = instance.distances[route[itr->first]][itr->second] + instance.distances[itr->second][route[itr->first + 1]]
                          - instance.distances[route[itr->first]][route[itr->first + 1]];
            }
        }
        itr++;
        while (itr != stationInserted.end())
        {
            int startInd, endInd;
            next = itr;
            next++;
            list<pair<int, int>>::iterator prev = itr;
            prev--;
            double sumdis = 0;
            if (next != stationInserted.end()) {
                startInd = prev->first + 1;
                endInd = next->first;
                sumdis += instance.distances[prev->second][route[startInd]];
                for (int i = startInd; i < endInd; i++) {
                    sumdis += instance.distances[route[i]][route[i + 1]];
                }
                sumdis += instance.distances[route[endInd]][next->second];
                if (sumdis <= instance.maxDis) {
                    double savedistemp = instance.distances[route[itr->first]][itr->second] + instance.distances[itr->second][route[itr->first + 1]]
                                         - instance.distances[route[itr->first]][route[itr->first + 1]];
                    if (savedistemp > savedis) {
                        savedis = savedistemp;
                        delone = itr;
                    }
                }
            }
            else {
                startInd = prev->first + 1;
                sumdis += instance.distances[prev->second][route[startInd]];
                for (int i = startInd; i < (int)route.size() - 1; i++) {
                    sumdis += instance.distances[route[i]][route[i + 1]];
                }
                if (sumdis <= instance.maxDis) {
                    double savedistemp = instance.distances[route[itr->first]][itr->second] + instance.distances[itr->second][route[itr->first + 1]]
                                         - instance.distances[route[itr->first]][route[itr->first + 1]];
                    if (savedistemp > savedis) {
                        savedis = savedistemp;
                        delone = itr;
                    }
                }
            }
            itr++;
        }
        if (savedis != 0) {
            stationInserted.erase(delone);
            changed = true;
        }
        if (!changed) {
            break;
        }
    }
    while (!stationInserted.empty())
    {
        route.insert(route.begin() + stationInserted.back().first + 1, stationInserted.back().second);
        stationInserted.pop_back();
    }
    double summ = 0;
    for (int i = 0; i < (int)route.size() - 1; i++) {
        summ += instance.distances[route[i]][route[i + 1]];
    }
    return make_pair(route, summ);
}

void LocalSearch::updateRouteData(Route * myRoute)
{
	int myplace = 0;
	double myload = 0.;
	double mytime = 0.;
	double myReversalDistance = 0.;
	double cumulatedX = 0.;
	double cumulatedY = 0.;
//修改的地方
    vector<int> r2({0});
	Node * mynode = myRoute->depot;
	mynode->position = 0;
	mynode->cumulatedLoad = 0.;
	mynode->cumulatedTime = 0.;
	mynode->cumulatedReversalDistance = 0.;

	bool firstIt = true;
	while (!mynode->isDepot || firstIt)
	{
		mynode = mynode->next;
		myplace++;
		mynode->position = myplace;
		myload += params.cli[mynode->cour].demand;
		mytime += params.timeCost[mynode->prev->cour][mynode->cour] + params.cli[mynode->cour].serviceDuration;
        r2.push_back(mynode->cour);//修改的地方
		myReversalDistance += params.timeCost[mynode->cour][mynode->prev->cour] - params.timeCost[mynode->prev->cour][mynode->cour] ;
		mynode->cumulatedLoad = myload;
		mynode->cumulatedTime = mytime;
		mynode->cumulatedReversalDistance = myReversalDistance;
		if (!mynode->isDepot)
		{
			cumulatedX += params.cli[mynode->cour].coordX;
			cumulatedY += params.cli[mynode->cour].coordY;
			if (firstIt) myRoute->sector.initialize(params.cli[mynode->cour].polarAngle);
			else myRoute->sector.extend(params.cli[mynode->cour].polarAngle);
		}
		firstIt = false;
	}

	myRoute->duration = mytime;
	myRoute->load = myload;
	myRoute->penalty = penaltyExcessDuration(mytime) + penaltyExcessLoad(myload);
	myRoute->nbCustomers = myplace-1;
	myRoute->reversalDistance = myReversalDistance;
	// Remember "when" this route has been last modified (will be used to filter unnecessary move evaluations)
	myRoute->whenLastModified = nbMoves ;

	if (myRoute->nbCustomers == 0)
	{
		myRoute->polarAngleBarycenter = 1.e30;
		emptyRoutes.insert(myRoute->cour);
	}
	else
	{
		myRoute->polarAngleBarycenter = atan2(cumulatedY/(double)myRoute->nbCustomers - params.cli[0].coordY, cumulatedX/(double)myRoute->nbCustomers - params.cli[0].coordX);
		emptyRoutes.erase(myRoute->cour);
	}
    //修改的地方
    myRoute->rrr=r2;
    if(r2.size()<=2)
        myRoute->fit_charge=INT_MAX;
    else
        myRoute->fit_charge=insertStationByRemove2(r2,params.c_evrp).second;//修改的地方
}

void LocalSearch::calRouteCharge(Route * myRoute)
{
    vector<int> r2({0});
    Node * mynode = myRoute->depot;
    bool firstIt = true;
    while (!mynode->isDepot || firstIt)
    {
        mynode = mynode->next;
        r2.push_back(mynode->cour);

        firstIt = false;
    }
    myRoute->fit_charge=insertStationByRemove2(r2,params.c_evrp).second;

}
// 将解决方案加载到LS中
void LocalSearch::loadIndividual(const Individual & indiv)
{
	emptyRoutes.clear();
	nbMoves = 0; 
	for (int r = 0; r < params.nbVehicles; r++) //nbVeh是由系统决定一个足够大的值
	{
		Node * myDepot = &depots[r];
		Node * myDepotFin = &depotsEnd[r];
		Route * myRoute = &routes[r];
		myDepot->prev = myDepotFin;
		myDepotFin->next = myDepot;
		if (!indiv.chromR[r].empty())
		{
			Node * myClient = &clients[indiv.chromR[r][0]];
			myClient->route = myRoute;
			myClient->prev = myDepot;
			myDepot->next = myClient;
			for (int i = 1; i < (int)indiv.chromR[r].size(); i++)
			{
				Node * myClientPred = myClient;
				myClient = &clients[indiv.chromR[r][i]]; 
				myClient->prev = myClientPred;
				myClientPred->next = myClient;
				myClient->route = myRoute;
			}
			myClient->next = myDepotFin;
			myDepotFin->prev = myClient;
		}
		else
		{
			myDepot->next = myDepotFin;
			myDepotFin->prev = myDepot;
		}
		updateRouteData(&routes[r]);
		routes[r].whenLastTestedSWAPStar = -1;
		for (int i = 1; i <= params.nbClients; i++) // Initializing memory structures
			bestInsertClient[r][i].whenLastCalculated = -1;
	}

	for (int i = 1; i <= params.nbClients; i++) // Initializing memory structures
		clients[i].whenLastTestedRI = -1;
}
// 将LS解决方案导出到个体，并根据Params中的原始惩罚权重计算惩罚成本
void LocalSearch::exportIndividual(Individual & indiv)
{
	std::vector < std::pair <double, int> > routePolarAngles ;
	for (int r = 0; r < params.nbVehicles; r++)
		routePolarAngles.push_back(std::pair <double, int>(routes[r].polarAngleBarycenter, r));
	std::sort(routePolarAngles.begin(), routePolarAngles.end()); // empty routes have a polar angle of 1.e30, and therefore will always appear at the end

	int pos = 0;
	for (int r = 0; r < params.nbVehicles; r++)
	{
		indiv.chromR[r].clear();
		Node * node = depots[routePolarAngles[r].second].next;
		while (!node->isDepot)
		{
			indiv.chromT[pos] = node->cour;
			indiv.chromR[r].push_back(node->cour);
			node = node->next;
			pos++;
		}
	}

	indiv.evaluateCompleteCost(params);
}

LocalSearch::LocalSearch(Params & params) : params (params),dis3(0,1)
{
	clients = std::vector < Node >(params.nbClients + 1);   // 客户+哨兵
	routes = std::vector < Route >(params.nbVehicles);  // 表示路线的元素
	depots = std::vector < Node >(params.nbVehicles);   // 表示仓库的元素
	depotsEnd = std::vector < Node >(params.nbVehicles);// 复制仓库以标记路线的终点
	bestInsertClient = std::vector < std::vector <ThreeBestInsert> >(params.nbVehicles, std::vector <ThreeBestInsert>(params.nbClients + 1));// (SWAP*) 对于每个路线和节点，存储最便宜的插入成本

	for (int i = 0; i <= params.nbClients; i++) 
	{
        // 0是哨兵
		clients[i].cour = i; 
		clients[i].isDepot = false; 
	}
	for (int i = 0; i < params.nbVehicles; i++)
	{
        // 路径初始化
		routes[i].cour = i;
		routes[i].depot = &depots[i];
        // 仓库节点初始化
		depots[i].cour = 0;
		depots[i].isDepot = true;
		depots[i].route = &routes[i];

		depotsEnd[i].cour = 0;
		depotsEnd[i].isDepot = true;
		depotsEnd[i].route = &routes[i];
	}
    // 加入客户节点
	for (int i = 1 ; i <= params.nbClients ; i++) orderNodes.push_back(i);
    // 加入路线
	for (int r = 0 ; r < params.nbVehicles ; r++) orderRoutes.push_back(r);
}

