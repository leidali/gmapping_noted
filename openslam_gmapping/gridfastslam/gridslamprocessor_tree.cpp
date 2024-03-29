#include <string>
#include <deque>
#include <list>
#include <map>
#include <set>
#include <fstream>
//#include <gsl/gsl_blas.h>

#include <gmapping/utils/stat.h>
#include "gmapping/gridfastslam/gridslamprocessor.h"

namespace GMapping {

using namespace std;

GridSlamProcessor::TNode::TNode(const OrientedPoint& p, double w, TNode* n, unsigned int c){
	pose=p;
	weight=w;
	childs=c;
	parent=n;
	reading=0;
	gweight=0;
	if (n){
		n->childs++;
	}
	flag=0;
	accWeight=0;
}


GridSlamProcessor::TNode::~TNode(){
	if (parent && (--parent->childs)<=0)
		delete parent;
	assert(!childs);
}


//BEGIN State Save/Restore

GridSlamProcessor::TNodeVector GridSlamProcessor::getTrajectories() const{
  TNodeVector v;
  TNodeMultimap parentCache;
  TNodeDeque border;
	
	for (ParticleVector::const_iterator it=m_particles.begin(); it!=m_particles.end(); it++){
		TNode* node=it->node;
		while(node){
			node->flag=false;
			node=node->parent;
		}
	}
	
	for (ParticleVector::const_iterator it=m_particles.begin(); it!=m_particles.end(); it++){
		TNode* newnode=new TNode(* (it->node) );
		
		v.push_back(newnode);
		assert(newnode->childs==0);
		if (newnode->parent){
			parentCache.insert(make_pair(newnode->parent, newnode));
			//cerr << __PRETTY_FUNCTION__ << ": node " << newnode->parent << " flag=" << newnode->parent->flag<< endl;
			if (! newnode->parent->flag){
				//cerr << __PRETTY_FUNCTION__ << ": node " << newnode->parent << " flag=" << newnode->parent->flag<< endl;
				newnode->parent->flag=true;
				border.push_back(newnode->parent);
			}
		}
	}
	
	//cerr << __PRETTY_FUNCTION__ << ": border.size(INITIAL)=" << border.size() << endl;
	//cerr << __PRETTY_FUNCTION__ << ": parentCache.size()=" << parentCache.size() << endl;
	while (! border.empty()){
		//cerr << __PRETTY_FUNCTION__ << ": border.size(PREPROCESS)=" << border.size() << endl;
		//cerr << __PRETTY_FUNCTION__ << ": parentCache.size(PREPROCESS)=" << parentCache.size() << endl;
		const TNode* node=border.front();
		//cerr << __PRETTY_FUNCTION__ << ": node " << node << endl;
		border.pop_front();
		if (! node)
			continue;
			
		TNode* newnode=new TNode(*node);
		node->flag=false;
		
		//update the parent of all of the referring childs 
		pair<TNodeMultimap::iterator, TNodeMultimap::iterator> p=parentCache.equal_range(node);
		double childs=0;
		for (TNodeMultimap::iterator it=p.first; it!=p.second; it++){
			assert(it->second->parent==it->first);
			(it->second)->parent=newnode;
			//cerr << "PS(" << it->first << ", "<< it->second << ")";
			childs++;
		}
		////cerr << endl;
		parentCache.erase(p.first, p.second);
		//cerr << __PRETTY_FUNCTION__ << ": parentCache.size(POSTERASE)=" << parentCache.size() << endl;
		assert(childs==newnode->childs);
		
		//unmark the node
		if ( node->parent ){
			parentCache.insert(make_pair(node->parent, newnode));
			if(! node->parent->flag){
				border.push_back(node->parent);
				node->parent->flag=true;
			}	
		}
		//insert the parent in the cache
	}
	//cerr << __PRETTY_FUNCTION__ << " : checking cloned trajectories" << endl;
	for (unsigned int i=0; i<v.size(); i++){
		TNode* node= v[i];
		while (node){
			//cerr <<".";
			node=node->parent;
		}
		//cerr << endl;
	}	
	
	return v;

}

void GridSlamProcessor::integrateScanSequence(GridSlamProcessor::TNode* node){
	//reverse the list 
	TNode* aux=node;
	TNode* reversed=0;
	double count=0;
	while(aux!=0){
		TNode * newnode=new TNode(*aux);
		newnode->parent=reversed;
		reversed=newnode;
		aux=aux->parent;
		count++;
	}
	
	//attach the path to each particle and compute the map;
	if (m_infoStream )
		m_infoStream << "Restoring State Nodes=" <<count << endl;
		
		
	aux=reversed;
	bool first=true;
	double oldWeight=0;
	OrientedPoint oldPose;
	while (aux!=0){
		if (first){
			oldPose=aux->pose;
			first=false;
			oldWeight=aux->weight;
		}
		
		OrientedPoint dp=aux->pose-oldPose;
		double dw=aux->weight-oldWeight;
		oldPose=aux->pose;
		
		
		double * plainReading = new double[m_beams];
		for(unsigned int i=0; i<m_beams; i++)
			plainReading[i]=(*(aux->reading))[i];
		
		for (ParticleVector::iterator it=m_particles.begin(); it!=m_particles.end(); it++){
			//compute the position relative to the path;
			double s=sin(oldPose.theta-it->pose.theta),
			       c=cos(oldPose.theta-it->pose.theta);
			
			it->pose.x+=c*dp.x-s*dp.y;
			it->pose.y+=s*dp.x+c*dp.y;
			it->pose.theta+=dp.theta;
			it->pose.theta=atan2(sin(it->pose.theta), cos(it->pose.theta));
			
			//register the scan
			m_matcher.invalidateActiveArea();
			m_matcher.computeActiveArea(it->map, it->pose, plainReading);
			it->weight+=dw;
			it->weightSum+=dw;

			// this should not work, since it->weight is not the correct weight!
			//			it->node=new TNode(it->pose, it->weight, it->node);
			it->node=new TNode(it->pose, 0.0, it->node);
			//update the weight
		}
		
		delete [] plainReading;
		aux=aux->parent;
	}
	
	//destroy the path
	aux=reversed;
	while (reversed){
		aux=reversed;
		reversed=reversed->parent;
		delete aux;
	}
}

//END State Save/Restore

//BEGIN

/// 更新粒子的权重
void  GridSlamProcessor::updateTreeWeights(bool weightsAlreadyNormalized)
{
  if (!weightsAlreadyNormalized) 
  {
    /*归一化粒子权重,并计算权重相似度Neff，用于判定是否需要进行重采样*/
    normalize(); 
  }
  
  /*重置轨迹树：各个节点的权重计数和访问计数器都设置为0*/
  resetTree();
  
  /*更新轨迹树权重：从叶子节点向上追溯到根节点，更新沿途经过的各个节点的权重和累积权重*/
  propagateWeights();
}

/// 重置轨迹树：各个节点的权重计数和访问计数器都设置为0
void GridSlamProcessor::resetTree()
{
  // don't calls this function directly, use updateTreeWeights(..) !

    // 遍历每个粒子
	for (ParticleVector::iterator it=m_particles.begin(); it!=m_particles.end(); it++)
    {
        // 遍历当前粒子的每个节点
		TNode* n=it->node;
		while (n)
        {
			n->accWeight=0;    // 权重
			n->visitCounter=0; // 访问次数
			n=n->parent;
		}
	}
}

/// 递归地更新轨迹树的权重，并返回最终根节点的权重
double propagateWeight(GridSlamProcessor::TNode* n, double weight)
{
    // 根节点
	if (!n)
		return weight;
    
	double w=0;
	n->visitCounter++;
	n->accWeight+=weight; // 子节点权重累加
    
    // 子节点都已经访问过了
	if (n->visitCounter==n->childs)
    {
		w=propagateWeight(n->parent,n->accWeight);
	}
    
	assert(n->visitCounter<=n->childs);
	return w;
}

/// 更新轨迹树权重：从叶子节点向上追溯到根节点，更新沿途经过的各个节点的权重和累积权重
double GridSlamProcessor::propagateWeights()
{
  // don't calls this function directly, use updateTreeWeights(..) !
  // all nodes must be resetted to zero and weights normalized

    // the accumulated weight of the root
    // 所有根节点的累计权重之和
	double lastNodeWeight=0;
    
	// sum of the weights in the leafs
    // 所有叶节点的累计权重之和
	double aw=0;

    /// 将m_weights数组里面的元素 分别作为每个粒子的叶节点权重，然后递归地更新轨迹树的权重
	std::vector<double>::iterator w=m_weights.begin();
	for (ParticleVector::iterator it=m_particles.begin(); it!=m_particles.end(); it++)
    {
        // 所有叶子节点的权重只和
		double weight=*w;
		aw+=weight;
        
        // 将m_weights数组里面的元素 分别作为每个粒子的叶节点权重
		TNode * n=it->node;
		n->accWeight=weight;
        
        // 递归地更新轨迹树的权重，并返回最终根节点的权重进行累加
		lastNodeWeight+=propagateWeight(n->parent,n->accWeight);
        
		w++;
	}
	
	if (fabs(aw-1.0) > 0.0001 || fabs(lastNodeWeight-1.0) > 0.0001) 
    {
	  cerr << "ERROR: ";
	  cerr << "root->accWeight=" << lastNodeWeight << "    sum_leaf_weights=" << aw << endl;
	  assert(0);         
	}
    
	return lastNodeWeight;
}

};

//END
