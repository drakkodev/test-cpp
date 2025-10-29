#include <iostream>
#include <vector>
#include <queue>
#include <climits>
using namespace std;

typedef pair<int, int> Par; // (distancia, nodo)

void dijkstra(int origen, vector<vector<Par>>& grafo) {
    int n = grafo.size();
    vector<int> dist(n, INT_MAX);
    priority_queue<Par, vector<Par>, greater<Par>> pq;

    dist[origen] = 0;
    pq.push({0, origen});

    while (!pq.empty()) {
        int u = pq.top().second;
        pq.pop();

        for (auto& vecino : grafo[u]) {
            int v = vecino.first;
            int peso = vecino.second;
            if (dist[u] + peso < dist[v]) {
                dist[v] = dist[u] + peso;
                pq.push({dist[v], v});
            }
        }
    }

    cout << "Distancias desde el nodo " << origen << ":\n";
    for (int i = 0; i < n; i++)
        cout << "Nodo " << i << " -> " << dist[i] << endl;
}

int main() {
    int n = 5;
    vector<vector<Par>> grafo(n);
    grafo[0] = {{1, 10}, {4, 5}};
    grafo[1] = {{2, 1}, {4, 2}};
    grafo[2] = {{3, 4}};
    grafo[3] = {};
    grafo[4] = {{1, 3}, {2, 9}, {3, 2}};

    dijkstra(0, grafo);
    return 0;
}
